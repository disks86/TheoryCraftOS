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

// Host-side tests for the GLSL preprocessor. Runs Mesa's glcpp corpus (tests/data/mesa/glcpp) the way Mesa's glcpp_test.py does: the messages followed by the output must
// equal the .expected file, with the input in each of the four newline styles. Cases in glcpp.xfail must fail, so that the list stays tight. On top of that: mutated corpus
// inputs (truncation, bit flips) under the sanitizers, the resource limits, #if arithmetic and the result fields of the interface.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "WebGl/GlslPreprocessor.hpp"

using namespace shader;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static const char kCorpus[] = "tests/data/mesa/glcpp";

static bool read_file(const std::string& path, std::string* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buffer[4096];
  size_t n;
  out->clear();
  while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) out->append(buffer, n);
  fclose(f);
  return true;
}

static std::string replace_newlines(const std::string& text, const std::string& newline) {
  std::string out;
  for (const char c : text) {
    if (c == '\n') out += newline;
    else out += c;
  }
  return out;
}

struct Run {
  std::string combined;  // the log, then the output: glcpp's stderr and stdout in the order it writes them
  std::string output, log;
  bool ok = false;
  uint32_t version = 0;
  bool version_declared = false, es = false;
  uint8_t profile = 0;
};

static Run run_preprocessor(const std::string& source, const PreprocessOptions& options = PreprocessOptions()) {
  TestAllocator allocator;
  Run run;
  {
    PreprocessResult result(allocator);
    const bool returned = preprocess_glsl(allocator, StringView(source.data(), source.size()), options, &result);
    CHECK(returned == result.ok);
    run.ok = result.ok;
    run.log.assign(result.log.data(), result.log.size());
    run.output.assign(result.output.data(), result.output.size());
    run.combined = run.log + run.output;
    run.version = result.version;
    run.version_declared = result.version_declared;
    run.es = result.es;
    run.profile = result.profile;
  }
  CHECK(allocator.live == 0);
  return run;
}

// Prints where two texts first differ.
static void print_difference(const std::string& actual, const std::string& expected) {
  size_t i = 0;
  while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
  const size_t from = i > 60 ? i - 60 : 0;
  printf("  first difference at byte %zu\n  actual:   [%s]\n  expected: [%s]\n", i, actual.substr(from, 160).c_str(), expected.substr(from, 160).c_str());
}

static std::vector<std::string> corpus_cases() {
  std::vector<std::string> names;
  DIR* dir = opendir(kCorpus);
  if (!dir) return names;
  while (dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.size() > 2 && name.compare(name.size() - 2, 2, ".c") == 0) names.push_back(name.substr(0, name.size() - 2));
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

// `case[@style] reason`, one per line; '#' starts a comment line.
static std::map<std::string, std::string> read_xfail_list() {
  std::map<std::string, std::string> list;
  std::string text;
  CHECK(read_file(std::string(kCorpus) + "/glcpp.xfail", &text));
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.empty() || line[0] == '#') continue;
    const size_t space = line.find(' ');
    list[line.substr(0, space)] = space == std::string::npos ? "" : line.substr(space + 1);
  }
  return list;
}

static PreprocessOptions options_for(const std::string& source) {
  PreprocessOptions options;
  const size_t args = source.find("glcpp-args:");
  if (args != std::string::npos && source.find("--disable-line-continuations", args) != std::string::npos) options.disable_line_continuation = true;
  return options;
}

static void test_corpus() {
  const std::vector<std::string> names = corpus_cases();
  const std::map<std::string, std::string> xfail = read_xfail_list();
  CHECK(names.size() >= 100);
  static const struct { const char* name; const char* newline; } kStyles[] = {{"unix", "\n"}, {"windows", "\r\n"}, {"oldmac", "\r"}, {"bizarro", "\n\r"}};
  int passed = 0, failed = 0, expected_failures = 0, unexpected_passes = 0;
  for (const std::string& name : names) {
    std::string source, expected;
    if (!read_file(std::string(kCorpus) + "/" + name + ".c", &source) || !read_file(std::string(kCorpus) + "/" + name + ".c.expected", &expected)) {
      printf("FAILED: cannot read case %s\n", name.c_str());
      ++failed;
      continue;
    }
    for (const auto& style : kStyles) {
      const std::string input = replace_newlines(source, style.newline);
      const Run run = run_preprocessor(input, options_for(source));
      const bool same = run.combined == expected;
      const std::string key = name + "@" + style.name;
      const bool listed = xfail.count(name) != 0 || xfail.count(key) != 0;
      if (same && !listed) {
        ++passed;
      } else if (same) {
        printf("FAILED: %s passes but is in glcpp.xfail\n", key.c_str());
        ++unexpected_passes;
      } else if (listed) {
        ++expected_failures;
      } else {
        printf("FAILED: %s\n", key.c_str());
        print_difference(run.combined, expected);
        ++failed;
      }
    }
  }
  // Every name on the list must be a case that exists.
  for (const auto& entry : xfail) {
    const std::string name = entry.first.substr(0, entry.first.find('@'));
    if (std::find(names.begin(), names.end(), name) == names.end()) {
      printf("FAILED: %s in glcpp.xfail is no case of the corpus\n", entry.first.c_str());
      ++failed;
    }
  }
  printf("glcpp corpus: %zu cases x 4 newline styles: %d passed, %d failed, %d xfail, %d unexpected passes\n", names.size(), passed, failed, expected_failures, unexpected_passes);
  g_failed += failed + unexpected_passes;
}

// Mutated inputs must not crash, trip a sanitizer, leak or stop on a bounds violation. The output is not checked.
static void test_mutations() {
  const std::vector<std::string> names = corpus_cases();
  uint64_t runs = 0;
  uint32_t state = 0x1234567u;
  const auto random = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  for (const std::string& name : names) {
    std::string source;
    if (!read_file(std::string(kCorpus) + "/" + name + ".c", &source)) continue;
    // Each mutation also picks the options, since ES and the lack of line continuations take other paths.
    const auto random_options = [&random]() {
      PreprocessOptions options;
      const uint32_t choice = random();
      options.disable_line_continuation = (choice & 1) != 0;
      if (choice & 2) {
        options.default_es = true;
        options.default_version = (choice & 4) ? 300 : 100;
      }
      return options;
    };
    for (size_t length = 0; length < source.size(); ++length) {
      run_preprocessor(source.substr(0, length), random_options());
      ++runs;
    }
    for (int i = 0; i < 200 && !source.empty(); ++i) {
      std::string mutated = source;
      const int flips = 1 + static_cast<int>(random() % 3);
      for (int f = 0; f < flips; ++f) mutated[random() % mutated.size()] ^= static_cast<char>(1u << (random() % 8));
      run_preprocessor(mutated, random_options());
      ++runs;
    }
    // A random run of bytes deleted or duplicated, which keeps tokens whole more often than a flipped bit does.
    for (int i = 0; i < 50 && !source.empty(); ++i) {
      std::string mutated = source;
      const size_t at = random() % mutated.size();
      const size_t count = 1 + random() % 16;
      if (random() & 1) mutated.erase(at, count);
      else mutated.insert(at, mutated.substr(at, count));
      run_preprocessor(mutated, random_options());
      ++runs;
    }
  }
  printf("mutated inputs: %llu runs\n", static_cast<unsigned long long>(runs));
}

static std::string lines_of(const std::string& first, int count, const std::string& line) {
  std::string out = first;
  for (int i = 0; i < count; ++i) out += line;
  return out;
}

// Expansion that grows without end must come back as a clean error, in reasonable time.
static void test_limits() {
  const auto is_limit_error = [](const Run& run) { return !run.ok && run.log.find("exceeds the limits of the preprocessor") != std::string::npos; };

  // The billion laughs.
  std::string laughs = "#define A0 lol lol lol lol lol lol lol lol lol lol\n";
  for (int i = 1; i < 40; ++i) {
    const std::string previous = "A" + std::to_string(i - 1);
    laughs += "#define A" + std::to_string(i) + " " + previous + " " + previous + " " + previous + " " + previous + "\n";
  }
  CHECK(is_limit_error(run_preprocessor(laughs + "A39\n")));

  // The same through function-like macros and arguments.
  std::string function_laughs = "#define F0(x) x x x x\n";
  for (int i = 1; i < 40; ++i) function_laughs += "#define F" + std::to_string(i) + "(x) F" + std::to_string(i - 1) + "(x x x x)\n";
  CHECK(is_limit_error(run_preprocessor(function_laughs + "F39(a)\n")));

  // Output that is large without many tokens: long names, many times over.
  const std::string long_name(1 << 16, 'n');
  std::string bomb = "#define L " + long_name + "\n#define L2 L L L L L L L L L L L L L L L L\n#define L3 L2 L2 L2 L2 L2 L2 L2 L2 L2 L2 L2 L2 L2 L2 L2 L2\n"
                     "#define L4 L3 L3 L3 L3 L3 L3 L3 L3 L3 L3 L3 L3 L3 L3 L3 L3\n";
  CHECK(is_limit_error(run_preprocessor(bomb + "L4 L4 L4 L4 L4 L4 L4 L4 L4 L4 L4 L4 L4 L4 L4 L4\n")));

  // Pasting that doubles a name at every step.
  std::string paste = "#define P0(x) x\n";
  for (int i = 1; i < 40; ++i) paste += "#define P" + std::to_string(i) + "(x) P" + std::to_string(i - 1) + "(x ## x)\n";
  CHECK(is_limit_error(run_preprocessor(paste + "P39(aaaaaaaaaaaaaaaa)\n")));

  // Macro calls nested in arguments, deeper than the stack may go.
  std::string nested = "#define F(x) x\n";
  for (int i = 0; i < 20000; ++i) nested += "F(";
  nested += "1";
  for (int i = 0; i < 20000; ++i) nested += ")";
  CHECK(is_limit_error(run_preprocessor(nested + "\n")));
  // A sensible depth is fine.
  nested = "#define F(x) x\n";
  for (int i = 0; i < 32; ++i) nested += "F(";
  nested += "1";
  for (int i = 0; i < 32; ++i) nested += ")";
  CHECK(run_preprocessor(nested + "\n").combined == "\n1\n");

  // Parentheses and prefixes in #if, on the heap and with an end.
  CHECK(run_preprocessor("#if " + std::string(100000, '(') + "1\n#endif\n").log.find("memory exhausted") != std::string::npos);
  CHECK(run_preprocessor("#if " + std::string(100000, '!') + "1\n#endif\n").log.find("memory exhausted") != std::string::npos);
  CHECK(run_preprocessor("#if " + std::string(1000, '(') + "1" + std::string(1000, ')') + "\nyes\n#endif\n").combined == "\nyes\n\n");

  // Quadratic traps, which must end either way: many parameters, and an unbalanced call rescanned many times.
  std::string parameters = "#define M(p0";
  for (int i = 1; i < 200000; ++i) parameters += ",p" + std::to_string(i);
  run_preprocessor(parameters + ") p0\n");
  run_preprocessor("#define F(x) x\n" + lines_of("", 200000, "F(") + "\n");
  run_preprocessor(lines_of("", 300000, "#if 1\n"));
  run_preprocessor(lines_of("", 300000, "#endif\n"));
  // A long chain of object-like macros, which keeps the whole chain on the stack of active macros.
  std::string chain;
  for (int i = 0; i < 20000; ++i) chain += "#define C" + std::to_string(i) + " C" + std::to_string(i + 1) + "\n";
  CHECK(run_preprocessor(chain + "C0\n").output.find("C20000") != std::string::npos);
  // Many definitions.
  std::string table;
  for (int i = 0; i < 100000; ++i) table += "#define T" + std::to_string(i) + " " + std::to_string(i) + "\n";
  CHECK(run_preprocessor(table + "T99999\n").output.find("99999") != std::string::npos);
  // Too many messages are dropped, not stored.
  CHECK(run_preprocessor(lines_of("", 300000, "#error too many\n")).log.size() <= (1u << 20));
}

static std::string error_text(const std::string& source) { return run_preprocessor(source).log; }

// Arithmetic with undefined behaviour in the C it was once written in. UBSan watches.
static void test_expressions() {
  const auto value_of = [](const std::string& expression) {
    return run_preprocessor("#if " + expression + "\nyes\n#else\nno\n#endif\n").output.find("yes") != std::string::npos;
  };
  CHECK(value_of("1"));
  CHECK(!value_of("0"));
  CHECK(value_of("2 + 3 * 4 == 14"));
  CHECK(value_of("(2 + 3) * 4 == 20"));
  CHECK(value_of("-1 < 0"));
  CHECK(value_of("~0 == -1"));
  CHECK(value_of("!0 && !!1"));
  CHECK(value_of("1 << 4 == 16"));
  CHECK(value_of("-16 >> 2 == -4"));
  CHECK(value_of("0x10 == 16 && 010 == 8 && 16u == 16"));
  CHECK(value_of("7 % 3 == 1 && 7 / 3 == 2 && -7 / 2 == -3 && -7 % 2 == -1"));
  CHECK(value_of("1 + 2 - 3 == 0"));
  CHECK(value_of("10 - 2 - 3 == 5"));  // left associative
  CHECK(value_of("- - 3 == 3"));
  CHECK(value_of("+ 3 == 3"));
  CHECK(value_of("1 | 2 ^ 3 & 4"));
  // Overflow, shift counts and the two divisions without a value: defined and no crash.
  CHECK(!run_preprocessor("#define MIN (-9223372036854775807 - 1)\n#if MIN / -1\n#endif\n").log.empty());
  CHECK(run_preprocessor("#define MIN (-9223372036854775807 - 1)\n#if MIN % -1\nyes\n#endif\n").output.find("yes") == std::string::npos);
  CHECK(value_of("9223372036854775807 + 1 < 0"));
  CHECK(value_of("9223372036854775807 * 2 == -2"));
  CHECK(value_of("-(-9223372036854775807 - 1) < 0"));
  CHECK(value_of("99999999999999999999999 == 9223372036854775807"));  // strtoll saturates
  CHECK(error_text("#if 1 << 64\n#endif\n").find("shift count out of range") != std::string::npos);
  CHECK(error_text("#if 1 << -1\n#endif\n").find("shift count out of range") != std::string::npos);
  CHECK(error_text("#if 1 >> 64\n#endif\n").find("shift count out of range") != std::string::npos);
  CHECK(value_of("(-1 >> 100) == -1"));
  CHECK(value_of("(1 >> 100) == 0"));
  CHECK(value_of("(1 << 100) == 0"));
  CHECK(error_text("#if 1 / 0\n#endif\n") == "0:1(10): preprocessor error: division by 0 in preprocessor directive\n");
  CHECK(error_text("#if 1 % 0\n#endif\n") == "0:1(10): preprocessor error: zero modulus in preprocessor directive\n");
  // No short-circuit for the arithmetic, as in the grammar.
  CHECK(error_text("#if 1 || 1 / 0\n#endif\n").find("division by 0") != std::string::npos);
  // Bison's messages.
  CHECK(error_text("#if 1 +\n#endif\n") == "0:1(8): preprocessor error: syntax error, unexpected NEWLINE\n");
  CHECK(error_text("#if (1\n#endif\n") == "0:1(7): preprocessor error: syntax error, unexpected NEWLINE\n");
  CHECK(error_text("#if 1)\n#endif\n") == "0:1(7): preprocessor error: syntax error, unexpected ')'\n");
  CHECK(error_text("#if 1 2\n#endif\n") == "0:1(8): preprocessor error: syntax error, unexpected INTEGER_STRING\n");
  // A division by zero is reduced ahead of the syntax error behind it.
  CHECK(error_text("#if 1 / 0 2\n#endif\n") == "0:1(12): preprocessor error: division by 0 in preprocessor directive\n0:1(12): preprocessor error: syntax error, unexpected INTEGER_STRING\n");
  CHECK(error_text("#undef 1\n") == "0:1(8): preprocessor error: syntax error, unexpected INTEGER_STRING, expecting IDENTIFIER\n");
  CHECK(error_text("#undef X Y\n") == "0:1(10): preprocessor error: syntax error, unexpected IDENTIFIER, expecting NEWLINE\n");
  CHECK(error_text("#define F(1)\n") == "0:1(11): preprocessor error: syntax error, unexpected INTEGER_STRING, expecting IDENTIFIER or ')'\n");
  CHECK(error_text("#define F(a b)\n") == "0:1(13): preprocessor error: syntax error, unexpected IDENTIFIER, expecting ')' or ','\n");
  CHECK(error_text("#define F(a,)\n") == "0:1(13): preprocessor error: syntax error, unexpected ')', expecting IDENTIFIER\n");
  CHECK(error_text("#version x\n") == "0:1(10): preprocessor error: syntax error, unexpected IDENTIFIER, expecting INTEGER_STRING\n");
  CHECK(error_text("#version 110 es x\n") == "0:1(17): preprocessor error: syntax error, unexpected IDENTIFIER, expecting NEWLINE\n");
  CHECK(error_text("#version 110 1\n") == "0:1(14): preprocessor error: syntax error, unexpected INTEGER_STRING, expecting IDENTIFIER or NEWLINE\n");
  CHECK(error_text("#version 0110\n") == "0:1(10): preprocessor error: invalid #version \"0110\" (not a decimal constant)\n");
  CHECK(error_text("#line x\n") == "0:1(8): preprocessor error: syntax error, unexpected IDENTIFIER, expecting INTEGER or INTEGER_STRING\n");
  CHECK(error_text("#line 1 x\n") == "0:1(10): preprocessor error: syntax error, unexpected IDENTIFIER, expecting INTEGER or INTEGER_STRING or NEWLINE or PATH\n");
  CHECK(error_text("#line 1 2 3\n") == "0:1(12): preprocessor error: syntax error, unexpected INTEGER_STRING, expecting NEWLINE\n");
  CHECK(error_text("#include <x.h>\n") == "0:1(1): preprocessor error: include <x.h> not found\n");
}

static void test_interface() {
  // No #version: the defaults.
  {
    const Run run = run_preprocessor("__VERSION__\n");
    CHECK(run.ok && run.output == "110\n" && run.version == 110 && !run.version_declared && !run.es && run.profile == 0);
  }
  {
    PreprocessOptions options;
    options.default_version = 100;
    options.default_es = true;
    const Run run = run_preprocessor("__VERSION__ GL_ES GL_FRAGMENT_PRECISION_HIGH GL_es_profile\n", options);
    CHECK(run.ok && run.output == "100 1 1 1\n" && run.version == 100 && !run.version_declared && run.es);
  }
  {
    PreprocessOptions options;
    options.default_version = 300;
    options.default_es = true;
    const Run run = run_preprocessor("__VERSION__ GL_ES\n", options);
    CHECK(run.output == "300 1\n" && run.es);
  }
  // #version.
  {
    const Run run = run_preprocessor("#version 300 es\n__VERSION__ GL_ES GL_es_profile GL_core_profile\n");
    CHECK(run.ok && run.output == "#version 300 es\n300 1 1 GL_core_profile\n" && run.version == 300 && run.version_declared && run.es && run.profile == 0);
  }
  {
    const Run run = run_preprocessor("#version 100\nGL_ES\n");
    CHECK(run.ok && run.es && run.output == "#version 100\n1\n");
  }
  {
    const Run run = run_preprocessor("#version 330 core\nGL_core_profile GL_ES GL_FRAGMENT_PRECISION_HIGH\n");
    CHECK(run.ok && run.version == 330 && !run.es && run.profile == 1 && run.output == "#version 330 core\n1 GL_ES 1\n");
  }
  {
    const Run run = run_preprocessor("#version 150 compatibility\nGL_compatibility_profile GL_core_profile\n");
    CHECK(run.ok && run.profile == 2 && run.output == "#version 150 compatibility\n1 GL_core_profile\n");
  }
  {
    const Run run = run_preprocessor("#version 130\nGL_core_profile GL_FRAGMENT_PRECISION_HIGH\n");
    CHECK(run.ok && run.output == "#version 130\nGL_core_profile 1\n");
  }
  // After another token it is an error, and the implicit version stays.
  {
    const Run run = run_preprocessor("int x;\n#version 300 es\n");
    CHECK(!run.ok && run.version == 110 && !run.version_declared && !run.es);
    CHECK(run.log == "0:2(1): preprocessor error: #version must appear on the first line\n");
  }
  // Extension macros.
  {
    const StringView extensions[] = {"GL_ARB_gpu_shader5", "GL_OES_standard_derivatives"};
    PreprocessOptions options;
    options.extension_macros = ArrayView<const StringView>(extensions, 2);
    const Run run = run_preprocessor("#ifdef GL_ARB_gpu_shader5\nGL_OES_standard_derivatives\n#endif\n", options);
    CHECK(run.ok && run.output == "\n1\n\n");
  }
  // Directives stay for the compiler's lexer, on their own lines.
  {
    const Run run = run_preprocessor("#extension GL_ARB_foo : enable\n#pragma optimize(on)\n#pragma\n#line 7\n__LINE__\n");
    CHECK(run.ok && run.output == "#extension GL_ARB_foo : enable\n#pragma optimize(on)\n\n#line 7\n7\n");
  }
  // The source ends at a NUL, as a C string does.
  CHECK(run_preprocessor(std::string("a\nb\0c\n", 6)).output == "a\nb\n");
  // Line continuation and its absence.
  CHECK(run_preprocessor("#define X a\\\nb\nX\n").output == "\n\nab\n");
  {
    PreprocessOptions options;
    options.disable_line_continuation = true;
    CHECK(!run_preprocessor("#define X a\\\nb\nX\n", options).output.empty());
  }
  // An empty source gives what glcpp gives: one newline.
  CHECK(run_preprocessor("").combined == "\n");
  // Called twice on one result: nothing stays behind.
  {
    TestAllocator allocator;
    {
      PreprocessResult result(allocator);
      CHECK(preprocess_glsl(allocator, "#version 300 es\nx\n", PreprocessOptions(), &result));
      CHECK(!preprocess_glsl(allocator, "#error x\n", PreprocessOptions(), &result));
      CHECK(!result.ok && result.version == 110 && !result.es && result.output.empty());
      CHECK(result.log == "0:1(1): preprocessor error: #error x\n");
    }
    CHECK(allocator.live == 0);
  }
  // An allocator that runs out must give a clean false, at any point.
  struct FailingAllocator final : allocators::IAllocator {
    int left;
    explicit FailingAllocator(int n) : left(n) {}
    int live = 0;
    void* allocate(uint64_t size) override {
      if (left <= 0) return nullptr;
      --left;
      ++live;
      return malloc(size);
    }
    void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
    void deallocate(void* p) override { if (p) { --live; free(p); } }
  };
  const char* const source = "#version 300 es\n#define F(x) x ## x\nint F(a); // comment\n#if defined(F) && 1\nF(b)\\\n c\n#endif\n#error x\n";
  bool succeeded = false;
  for (int budget = 0; budget < 200 && !succeeded; ++budget) {
    FailingAllocator allocator(budget);
    {
      PreprocessResult result(allocator);
      preprocess_glsl(allocator, source, PreprocessOptions(), &result);
      succeeded = result.log.view().contains("#error x");
    }
    CHECK(allocator.live == 0);
  }
  CHECK(succeeded);
}

int main() {
  test_corpus();
  test_interface();
  test_expressions();
  test_limits();
  test_mutations();
  if (g_failed) {
    printf("GlcppTest: %d failures\n", g_failed);
    return 1;
  }
  printf("GlcppTest: all passed\n");
  return 0;
}
