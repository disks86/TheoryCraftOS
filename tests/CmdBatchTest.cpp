// TheoryCraftOS - a minimal UEFI x86_64 operating system.
// Copyright (C) 2026 Christopher Schaefer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// Host test of cmd.exe's interpreter (src/Windows/cmd/batch.inc, freestanding C for Windows) over a pretend Windows: files and folders in maps, an
// environment, a current folder, and programs that are recorded instead of started. Unigine Heaven's launcher batch file, expansion (%~dp0 and the
// other modifiers, variables, edits), the built-in commands, labels, CALL, IF, && and ||, redirection, PATH and PATHEXT, and truncation and
// single-character mutations of a batch file for the sanitizers.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

typedef char16_t WCHAR;
#include "../src/Windows/cmd/batch.inc"

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++g_failed; } } while (0)

static std::u16string u(const char* s) { std::u16string r; while (*s) r.push_back((char16_t)(unsigned char)*s++); return r; }
static std::string a(const std::u16string& s) { std::string r; for (char16_t c : s) r.push_back(c < 128 ? (char)c : '?'); return r; }
static std::u16string lower(std::u16string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char16_t)(c + 32); return s; }

struct Run { std::u16string program, command_line, directory; int wait, output; };

// A pretend Windows. Paths are kept as written, compared without case.
struct World {
  std::map<std::u16string, std::u16string> files;      // lower-case path -> text
  std::set<std::u16string> folders;                    // lower-case paths
  std::map<std::u16string, std::pair<std::u16string, std::u16string>> env;      // lower-case name -> (name, value)
  std::u16string cwd = u("C:\\Users\\Default");
  std::u16string out, err;
  std::map<int, std::u16string> streams;               // open redirection files: stream -> path
  std::map<int, int> stream_modes;
  int next_stream = 3;
  std::vector<Run> runs;
  std::map<std::u16string, unsigned> exit_codes;       // lower-case program path -> its exit code
  std::vector<std::u16string> input;                   // lines read_line gives
  long blocks = 0;

  void folder(const char* p) {
    std::u16string path = u(p);
    for (size_t i = 3; i <= path.size(); ++i) if (i == path.size() || path[i] == '\\') folders.insert(lower(path.substr(0, i)));
    folders.insert(lower(path.substr(0, 3)));
  }
  void file(const char* p, const char* text) {
    std::u16string path = u(p);
    folder(a(path.substr(0, path.rfind('\\'))).c_str());
    files[lower(path)] = u(text);
  }
  void set(const char* name, const char* value) { env[lower(u(name))] = {u(name), u(value)}; }

  // The full path the way GetFullPathNameW makes it.
  std::u16string full(const std::u16string& in) const {
    std::u16string p = in;
    for (auto& c : p) if (c == '/') c = '\\';
    std::u16string base;
    if (p.size() >= 2 && p[1] == ':') { base = p.substr(0, 2); p = p.substr(2); if (p.empty() || p[0] != '\\') p = (lower(base) == lower(cwd.substr(0, 2)) ? cwd.substr(2) : u("")) + u("\\") + p; }
    else if (!p.empty() && p[0] == '\\') base = cwd.substr(0, 2);
    else { base = cwd.substr(0, 2); p = cwd.substr(2) + u("\\") + p; }
    std::vector<std::u16string> parts;
    size_t i = 0;
    while (i <= p.size()) {
      size_t j = p.find('\\', i);
      if (j == std::u16string::npos) j = p.size();
      std::u16string part = p.substr(i, j - i);
      while (!part.empty() && (part.back() == ' ' || part.back() == '.') && part != u("..") && part != u(".")) part.pop_back();
      if (part == u("..")) { if (!parts.empty()) parts.pop_back(); }
      else if (!part.empty() && part != u(".")) parts.push_back(part);
      i = j + 1;
    }
    std::u16string r = base + u("\\");
    for (size_t k = 0; k < parts.size(); ++k) { if (k) r += u("\\"); r += parts[k]; }
    return r;
  }
};

static World* W(void* c) { return static_cast<World*>(c); }
static void* h_alloc(void* c, unsigned long bytes) { ++W(c)->blocks; return calloc(1, bytes); }
static void h_release(void* c, void* b) { if (b) --W(c)->blocks; free(b); }
static int put(const std::u16string& s, WCHAR* out, int capacity) {
  if ((int)s.size() >= capacity) return -1;
  for (size_t i = 0; i < s.size(); ++i) out[i] = s[i];
  out[s.size()] = 0;
  return (int)s.size();
}
static int h_get_variable(void* c, const WCHAR* name, WCHAR* out, int capacity) {
  auto it = W(c)->env.find(lower(name));
  if (it == W(c)->env.end()) return -1;
  return put(it->second.second, out, capacity);
}
static int h_set_variable(void* c, const WCHAR* name, const WCHAR* value) {
  if (!value) W(c)->env.erase(lower(name));
  else W(c)->env[lower(name)] = {name, value};
  return 1;
}
static int h_current_directory(void* c, WCHAR* out, int capacity) { const int n = put(W(c)->cwd, out, capacity); return n < 0 ? 0 : n; }
static int h_change_directory(void* c, const WCHAR* path) {
  if (!W(c)->folders.count(lower(path))) return 0;
  W(c)->cwd = path;
  return 1;
}
static int h_full_path(void* c, const WCHAR* path, WCHAR* out, int capacity) { const int n = put(W(c)->full(path), out, capacity); return n < 0 ? 0 : n; }
static int h_file_kind(void* c, const WCHAR* path) {
  const std::u16string p = lower(path);
  if (W(c)->files.count(p)) return BATCH_KIND_FILE;
  if (W(c)->folders.count(p)) return BATCH_KIND_FOLDER;
  return BATCH_KIND_NONE;
}
static int h_run(void* c, const BatchRun* r, unsigned* code) {
  W(c)->runs.push_back({r->program, r->command_line, r->directory, r->wait, r->output});
  auto it = W(c)->exit_codes.find(lower(r->program));
  *code = it == W(c)->exit_codes.end() ? 0 : it->second;
  return 1;
}
static void h_write(void* c, int stream, const WCHAR* text, int length) {
  std::u16string s(text, (size_t)length);
  if (stream == BATCH_OUT) W(c)->out += s;
  else if (stream == BATCH_ERR) W(c)->err += s;
  else if (W(c)->streams.count(stream)) W(c)->files[lower(W(c)->streams[stream])] += s;
}
static int h_open_stream(void* c, const WCHAR* path, int mode) {
  const std::u16string full = W(c)->full(path);
  if (mode == BATCH_READ && !W(c)->files.count(lower(full))) return -1;
  if (mode == BATCH_WRITE) W(c)->files[lower(full)] = u("");
  if (mode == BATCH_APPEND && !W(c)->files.count(lower(full))) W(c)->files[lower(full)] = u("");
  const int s = W(c)->next_stream++;
  W(c)->streams[s] = full;
  W(c)->stream_modes[s] = mode;
  return s;
}
static void h_close_stream(void* c, int stream) { W(c)->streams.erase(stream); }
static int h_load_script(void* c, const WCHAR* path, WCHAR** text, int* length) {
  auto it = W(c)->files.find(lower(path));
  if (it == W(c)->files.end()) return 0;
  WCHAR* block = static_cast<WCHAR*>(h_alloc(c, (it->second.size() + 1) * sizeof(WCHAR)));
  if (!block) return 0;
  for (size_t i = 0; i < it->second.size(); ++i) block[i] = it->second[i];
  *text = block;
  *length = (int)it->second.size();
  return 1;
}
static int h_read_line(void* c, WCHAR* out, int capacity) {
  if (W(c)->input.empty()) return -1;
  const std::u16string line = W(c)->input.front();
  W(c)->input.erase(W(c)->input.begin());
  const int n = put(line, out, capacity);
  return n < 0 ? 0 : n;
}
static int h_type_file(void* c, const WCHAR* path, int stream) {
  auto it = W(c)->files.find(lower(path));
  if (it == W(c)->files.end()) return 0;
  h_write(c, stream, it->second.data(), (int)it->second.size());
  return 1;
}

struct Cmd {
  World world;
  BatchHost host;
  BatchShell shell;
  Cmd() {
    host = BatchHost{&world, h_alloc, h_release, h_get_variable, h_set_variable, h_current_directory, h_change_directory, h_full_path, h_file_kind,
                     h_run, h_write, h_open_stream, h_close_stream, h_load_script, h_read_line, h_type_file};
    world.folder("C:\\Users\\Default");
    world.folder("C:\\Windows\\System32");
    world.set("PATH", "C:\\Windows\\System32;C:\\Windows");
    world.set("ComSpec", "C:\\Windows\\System32\\cmd.exe");
    world.file("C:\\Windows\\System32\\cmd.exe", "MZ");
    batch_shell_init(&shell, &host);
  }
  unsigned run(const char* line) {
    const std::u16string text = u(line);
    return batch_run_command(&shell, bv(text.data(), (int)text.size()));
  }
};

// Unigine Heaven's Start menu entry: cmd.exe /c "C:\Program Files\Unigine\Heaven Benchmark 4.0\heaven.bat".
static void test_heaven() {
  Cmd cmd;
  cmd.world.file("C:\\Program Files\\Unigine\\Heaven Benchmark 4.0\\heaven.bat",
                 "@echo off\r\ncd %~dp0/bin\r\nstart browser_x86.exe -config ../data/launcher/launcher.xml\r\n");
  cmd.world.file("C:\\Program Files\\Unigine\\Heaven Benchmark 4.0\\bin\\browser_x86.exe", "MZ");
  cmd.world.cwd = u("C:\\Program Files\\Unigine\\Heaven Benchmark 4.0");
  const unsigned code = cmd.run("\"C:\\Program Files\\Unigine\\Heaven Benchmark 4.0\\heaven.bat\"");
  CHECK(code == 0);
  CHECK(cmd.world.out.empty());
  if (!cmd.world.err.empty()) printf("  errors: %s\n", a(cmd.world.err).c_str());
  CHECK(cmd.world.err.empty());
  CHECK(cmd.world.cwd == u("C:\\Program Files\\Unigine\\Heaven Benchmark 4.0\\bin"));
  CHECK(cmd.world.runs.size() == 1);
  if (cmd.world.runs.size() == 1) {
    const Run& r = cmd.world.runs[0];
    CHECK(r.program == u("C:\\Program Files\\Unigine\\Heaven Benchmark 4.0\\bin\\browser_x86.exe"));
    CHECK(r.command_line == u("\"C:\\Program Files\\Unigine\\Heaven Benchmark 4.0\\bin\\browser_x86.exe\" -config ../data/launcher/launcher.xml"));
    CHECK(r.directory == u("C:\\Program Files\\Unigine\\Heaven Benchmark 4.0\\bin"));
    CHECK(r.wait == 0);
  }
  CHECK(cmd.world.blocks == 0);
}

static void test_echo() {
  Cmd cmd;
  cmd.world.file("C:\\b\\e.bat", "echo hello  world\r\necho.\r\n@echo off\r\necho\r\necho on\r\n@echo:x\r\nrem nothing\r\n:: a comment\r\n:label\r\n");
  cmd.world.cwd = u("C:\\b");
  cmd.run("e.bat");
  const std::string out = a(cmd.world.out);
  CHECK(out == "\r\nC:\\b>echo hello  world\r\nhello  world\r\n\r\nC:\\b>echo.\r\n\r\nECHO is off.\r\nx\r\n\r\nC:\\b>rem nothing\r\n");      // (ECHO ON again: REM is shown, "::" and labels are not)
  if (out != "\r\nC:\\b>echo hello  world\r\nhello  world\r\n\r\nC:\\b>echo.\r\n\r\nECHO is off.\r\nx\r\n\r\nC:\\b>rem nothing\r\n") printf("  out: [%s]\n", out.c_str());
}

static void test_expansion() {
  Cmd cmd;
  cmd.world.file("C:\\Games\\My Game\\run it.bat",
                 "@echo off\r\n"
                 "echo [%0] [%~0] [%~dp0] [%~n0] [%~x0] [%~f0] [%~d0] [%~nx0]\r\n"
                 "echo [%1] [%~1] [%2] [%*] [%9]\r\n"
                 "set NAME=Hello World\r\n"
                 "echo [%NAME%] [%NAME:~6%] [%NAME:~0,5%] [%NAME:~-3%] [%NAME:o=0%] [%NAME:*o=X%] [%MISSING%] [100%%] [%]\r\n"
                 "shift\r\n"
                 "echo [%1] [%~1]\r\n"
                 "echo [%CD%] [%ERRORLEVEL%]\r\n");
  cmd.world.cwd = u("C:\\Users\\Default");
  cmd.run("\"C:\\Games\\My Game\\run it.bat\" \"first arg\" second");
  const std::string out = a(cmd.world.out);
  const char* expected =
      "[\"C:\\Games\\My Game\\run it.bat\"] [C:\\Games\\My Game\\run it.bat] [C:\\Games\\My Game\\] [run it] [.bat] [C:\\Games\\My Game\\run it.bat] [C:] [run it.bat]\r\n"
      "[\"first arg\"] [first arg] [second] [\"first arg\" second] []\r\n"
      "[Hello World] [World] [Hello] [rld] [Hell0 W0rld] [X World] [] [100%] []\r\n"
      "[second] [second]\r\n"
      "[C:\\Users\\Default] [0]\r\n";
  if (out != expected) printf("  out: [%s]\n", out.c_str());
  CHECK(out == expected);
  // a command line (not a batch file) keeps what it cannot expand
  Cmd line;
  line.run("echo %MISSING% 50% %PATH:~0,10%");
  CHECK(a(line.world.out) == "%MISSING% 50% C:\\Windows\r\n");
}

static void test_control_flow() {
  Cmd cmd;
  cmd.world.file("C:\\b\\flow.bat",
                 "@echo off\r\n"
                 "goto skip\r\n"
                 "echo not shown\r\n"
                 ":skip\r\n"
                 "call :sub one two\r\n"
                 "echo after sub %ERRORLEVEL%\r\n"
                 "if errorlevel 3 echo level3\r\n"
                 "if errorlevel 4 echo level4\r\n"
                 "if not errorlevel 4 echo below4\r\n"
                 "if exist C:\\b\\flow.bat echo exists\r\n"
                 "if not exist C:\\b\\nothing.txt echo missing\r\n"
                 "if \"%~nx0\"==\"flow.bat\" echo same\r\n"
                 "if /i abc==ABC echo nocase\r\n"
                 "if abc==ABC echo wrong\r\n"
                 "if defined PATH echo defined\r\n"
                 "echo a & echo b\r\n"
                 "nosuchprogram 2>nul && echo bad || echo failed %ERRORLEVEL%\r\n"
                 "call other.bat x\r\n"
                 "echo back\r\n"
                 "goto :eof\r\n"
                 ":sub\r\n"
                 "echo in sub %1 %2 %~dp0\r\n"
                 "exit /b 3\r\n");
  cmd.world.file("C:\\b\\other.bat", "@echo other %1\r\n");
  cmd.world.cwd = u("C:\\b");
  const unsigned code = cmd.run("flow.bat");
  const std::string out = a(cmd.world.out);
  const char* expected =
      "in sub one two C:\\b\\\r\n"
      "after sub 3\r\n"
      "level3\r\n"
      "below4\r\n"
      "exists\r\n"
      "missing\r\n"
      "same\r\n"
      "nocase\r\n"
      "defined\r\n"
      "a\r\n"
      "b\r\n"
      "failed 3\r\n"      // (%ERRORLEVEL% was expanded before the line ran, as in cmd)
      "other x\r\n"
      "back\r\n";
  if (out != expected) printf("  out: [%s]\n  err: [%s]\n", out.c_str(), a(cmd.world.err).c_str());
  CHECK(out == expected);
  CHECK(code == 9009);      // (the ERRORLEVEL the batch file ended with: ECHO does not reset it)
  CHECK(cmd.world.blocks == 0);

  // A batch file named without CALL takes over: the first one does not go on. EXIT (no /B) ends everything with its code.
  Cmd chain;
  chain.world.file("C:\\b\\first.bat", "@echo off\r\nsecond.bat\r\necho not reached\r\n");
  chain.world.file("C:\\b\\second.bat", "@echo second\r\n@exit 7\r\n@echo not reached either\r\n");
  chain.world.cwd = u("C:\\b");
  CHECK(chain.run("first.bat") == 7);
  CHECK(a(chain.world.out) == "second\r\n");

  // A label that is not there ends the batch file with a message.
  Cmd nolabel;
  nolabel.world.file("C:\\b\\n.bat", "@goto nowhere\r\n@echo no\r\n");
  nolabel.world.cwd = u("C:\\b");
  nolabel.run("n.bat");
  CHECK(nolabel.world.out.empty() && a(nolabel.world.err).find("cannot find the batch label") != std::string::npos);
}

static void test_programs() {
  Cmd cmd;
  cmd.world.file("C:\\Tools\\tool.exe", "MZ");
  cmd.world.file("C:\\b\\local.com", "MZ");
  cmd.world.file("C:\\b\\script.cmd", "@echo script\r\n");
  cmd.world.set("PATH", "C:\\Windows\\System32;\"C:\\Tools\";;");
  cmd.world.exit_codes[lower(u("C:\\Tools\\tool.exe"))] = 5;
  cmd.world.cwd = u("C:\\b");
  CHECK(cmd.run("tool --flag \"a b\"") == 5);
  CHECK(cmd.world.runs.size() == 1 && cmd.world.runs[0].command_line == u("\"C:\\Tools\\tool.exe\" --flag \"a b\"") && cmd.world.runs[0].wait == 1);
  CHECK(cmd.run("local") == 0 && cmd.world.runs.size() == 2 && cmd.world.runs[1].program == u("C:\\b\\local.com"));
  CHECK(cmd.run("script") == 0 && a(cmd.world.out) == "script\r\n");      // (.cmd through PATHEXT, run by this interpreter)
  CHECK(cmd.run("nothing.exe") == 9009);
  CHECK(a(cmd.world.err).find("'nothing.exe' is not recognized") != std::string::npos);
  // START: a title, /WAIT, /D, a batch file through %ComSpec%
  cmd.world.runs.clear();
  CHECK(cmd.run("start \"My Title\" /wait tool x") == 5);
  CHECK(cmd.world.runs.size() == 1 && cmd.world.runs[0].wait == 1 && cmd.world.runs[0].command_line == u("\"C:\\Tools\\tool.exe\" x"));
  cmd.world.folder("C:\\Other");
  cmd.world.file("C:\\Other\\there.exe", "MZ");
  CHECK(cmd.run("start /d C:\\Other there.exe") == 0);
  CHECK(cmd.world.runs.size() == 2 && cmd.world.runs[1].directory == u("C:\\Other") && cmd.world.runs[1].wait == 0 && cmd.world.cwd == u("C:\\b"));
  CHECK(cmd.run("start script.cmd arg") == 0);
  CHECK(cmd.world.runs.size() == 3 && cmd.world.runs[2].program == u("C:\\Windows\\System32\\cmd.exe") &&
        cmd.world.runs[2].command_line == u("\"C:\\Windows\\System32\\cmd.exe\" /c \"\"C:\\b\\script.cmd\" arg\""));
  CHECK(cmd.run("start nothing.exe") == 9059);
  // a program's output to NUL and to a file
  CHECK(cmd.run("tool >nul 2>&1") == 5 && cmd.world.runs.back().output == BATCH_NUL);
  CHECK(cmd.run("tool > out.txt") == 5 && cmd.world.runs.back().output >= 3);
}

static void test_folders_and_variables() {
  Cmd cmd;
  cmd.world.folder("C:\\One\\Two Words\\Three");
  cmd.world.cwd = u("C:\\One");
  cmd.run("cd Two Words/Three");
  CHECK(cmd.world.cwd == u("C:\\One\\Two Words\\Three"));
  cmd.run("cd ..\\..");
  CHECK(cmd.world.cwd == u("C:\\One"));
  cmd.run("cd \"C:\\One\\Two Words\"");
  CHECK(cmd.world.cwd == u("C:\\One\\Two Words"));
  cmd.run("cd..");
  CHECK(cmd.world.cwd == u("C:\\One"));
  cmd.run("cd \\");
  CHECK(cmd.world.cwd == u("C:\\"));
  CHECK(cmd.run("cd C:\\Nowhere") == 1 && cmd.world.cwd == u("C:\\"));
  CHECK(a(cmd.world.err).find("cannot find the path") != std::string::npos);
  cmd.world.out.clear();
  cmd.run("cd");
  CHECK(a(cmd.world.out) == "C:\\\r\n");
  cmd.run("pushd C:\\One\\Two Words");
  CHECK(cmd.world.cwd == u("C:\\One\\Two Words"));
  cmd.run("popd");
  CHECK(cmd.world.cwd == u("C:\\"));
  cmd.run("C:");
  CHECK(cmd.world.cwd == u("C:\\"));
  CHECK(cmd.run("D:") == 1);

  cmd.run("set GREETING=hi there");
  CHECK(cmd.world.env[u("greeting")].second == u("hi there"));
  cmd.run("set \"QUOTED=a & b\" ignored");
  CHECK(cmd.world.env[u("quoted")].second == u("a & b"));
  cmd.run("set GREETING=");
  CHECK(!cmd.world.env.count(u("greeting")));
  cmd.world.out.clear();
  cmd.run("set QUOTED");
  CHECK(a(cmd.world.out) == "QUOTED=a & b\r\n");
  CHECK(cmd.run("set /a 1+1") == 1);

  // ECHO into a file, appended to, read back with TYPE
  cmd.world.cwd = u("C:\\One");
  cmd.run("echo first> C:\\One\\log.txt");
  cmd.run("echo second>>C:\\One\\log.txt");
  CHECK(a(cmd.world.files[lower(u("C:\\One\\log.txt"))]) == "first\r\nsecond\r\n");
  cmd.world.out.clear();
  cmd.run("type log.txt");
  CHECK(a(cmd.world.out) == "first\r\nsecond\r\n");
  cmd.run("echo gone >nul");
  CHECK(a(cmd.world.out) == "first\r\nsecond\r\n");
  cmd.world.out.clear();
  cmd.run("echo ^& ^> \"q & >\"");
  CHECK(a(cmd.world.out) == "& > \"q & >\"\r\n");
  CHECK(cmd.run("echo a | more") == 1 && a(cmd.world.err).find("Pipes") != std::string::npos);
  CHECK(cmd.world.streams.empty());

  // PAUSE reads a line; the interactive shell runs lines until EXIT
  Cmd interactive;
  interactive.world.input = {u("echo one"), u("pause"), u(""), u("exit 4"), u("echo never")};
  CHECK(batch_interactive(&interactive.shell) == 4);
  CHECK(a(interactive.world.out) == "C:\\Users\\Default>one\r\n\r\nC:\\Users\\Default>Press any key to continue . . . \r\n\r\nC:\\Users\\Default>");      // (nothing after EXIT)
  CHECK(interactive.world.input.size() == 1);
}

// Every truncation and every single-character change of a batch file runs to its end (or a GOTO loop's guard) without a sanitizer report.
static void test_damage() {
  const char* script =
      "@echo off\r\nset A=%~dp0x\r\nif \"%A%\"==\"\" goto :end\r\ncd %~dp0/bin\r\nstart \"t\" /d .. browser_x86.exe -config ../x.xml >nul 2>&1\r\n"
      "call :sub %1 \"two\" & echo %A:~1,3% %A:x=y% || exit /b 2\r\n:sub\r\necho %* > out.txt\r\n:end\r\n";
  const std::string base(script);
  const char replacements[] = {'%', '"', '^', '&', '|', '>', '<', ':', '~', '\n', '\r', ' ', '=', '0', 'x', '\\', '/', '(', '*'};
  long cases = 0;
  auto run_one = [&](const std::string& text) {
    Cmd cmd;
    cmd.world.file("C:\\G\\heaven.bat", "");
    cmd.world.files[lower(u("C:\\G\\heaven.bat"))] = u(text.c_str());
    cmd.world.file("C:\\G\\bin\\browser_x86.exe", "MZ");
    cmd.world.cwd = u("C:\\G");
    cmd.run("heaven.bat arg1");
    CHECK(cmd.world.blocks == 0);
    ++cases;
  };
  for (size_t n = 0; n <= base.size(); ++n) run_one(base.substr(0, n));
  for (size_t i = 0; i < base.size(); ++i)
    for (char r : replacements) {
      std::string t = base;
      t[i] = r;
      if (t.find("goto") != std::string::npos && t.find(":end") == std::string::npos && t.find(":sub") == std::string::npos) continue;
      run_one(t);
    }
  // a line longer than cmd's limit is refused whole, and the next one still runs
  std::string longline = "echo " + std::string(BATCH_LINE + 10, 'x') + "\r\necho after\r\n";
  Cmd cmd;
  cmd.world.file("C:\\b\\long.bat", "");
  cmd.world.files[lower(u("C:\\b\\long.bat"))] = u(longline.c_str());
  cmd.world.cwd = u("C:\\b");
  cmd.shell.echo = 0;
  cmd.run("long.bat");
  CHECK(a(cmd.world.out) == "after\r\n" && a(cmd.world.err).find("too long") != std::string::npos);
  // a variable that expands past the limit
  Cmd big;
  big.world.set("BIG", std::string(6000, 'y').c_str());
  big.world.file("C:\\b\\big.bat", "@echo %BIG%%BIG%\r\n@echo ok\r\n");
  big.world.cwd = u("C:\\b");
  big.run("big.bat");
  CHECK(a(big.world.out) == "ok\r\n");
  // CALL inside itself stops at the depth limit
  Cmd deep;
  deep.world.file("C:\\b\\self.bat", "@call self.bat\r\n");
  deep.world.cwd = u("C:\\b");
  deep.run("self.bat");
  CHECK(a(deep.world.err).find("too deeply") != std::string::npos && deep.world.blocks == 0);
  printf("  %ld damaged batch files\n", cases);
}

int main() {
  test_heaven();
  test_echo();
  test_expansion();
  test_control_flow();
  test_programs();
  test_folders_and_variables();
  test_damage();
  if (g_failed) {
    printf("CmdBatchTest: %d check(s) FAILED\n", g_failed);
    return 1;
  }
  printf("CmdBatchTest: all checks passed\n");
  return 0;
}
