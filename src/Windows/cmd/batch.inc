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

/* The command interpreter of cmd.exe: batch files (.bat, .cmd) and single command lines, as Windows' cmd reads them.
 *
 * What it does:
 *   - lines: "@" (the line is not echoed), labels and "::" comments, ECHO ON/OFF and the echo of each command with the prompt ("C:\dir>");
 *   - expansion: %% %0..%9 %* %~0 and the modifiers f d p n x (%~dp0: the drive and folder of the batch file), %NAME%, %NAME:~start,length%,
 *     %NAME:old=new%, the dynamic %CD% and %ERRORLEVEL%; "^" escapes, quotes;
 *   - commands joined by "&", "&&" and "||"; redirection of output, errors and input to a file or NUL (">", ">>", "2>", "2>&1", "<");
 *   - built-in commands: REM ECHO CD/CHDIR "X:" SET START CALL (a batch file or a :label) GOTO EXIT [/B] IF ([NOT] EXIST, ERRORLEVEL, DEFINED, ==,
 *     /I) SHIFT PUSHD POPD PAUSE TYPE VER and, doing nothing, TITLE CLS COLOR SETLOCAL ENDLOCAL VERIFY;
 *   - a program: looked for in the current folder, then the folders of PATH, with the extensions of PATHEXT; it runs and is waited for, and its exit
 *     code is ERRORLEVEL (9009 when it is not found). Another batch file named as a command takes over from the one running, as in cmd.
 * What it does not do: blocks in parentheses (and with them ELSE and FOR), pipes, SET /A and /P, delayed expansion (!VAR!), SETLOCAL's scope. A line
 * that needs them says so ("... is not supported") and the batch file goes on with the next line.
 *
 * Everything outside - files, folders, the environment, starting programs, the console - is the host's (BatchHost), so this file has no Windows in it
 * and is tested on a development machine (tests/CmdBatchTest.cpp). Text is UTF-16 (WCHAR, an unsigned 16-bit type the includer defines). Every buffer
 * has a fixed size and every write into it is checked (BText); a line longer than cmd's limit (8191 characters) is refused, not cut. The interpreter
 * allocates only its frames, through the host. */

#define BATCH_LINE 8191           /* the longest line, as in cmd */
#define BATCH_PATH 1024           /* the longest path a command works with */
#define BATCH_DEPTH 16            /* batch files and :labels CALLed inside each other */
#define BATCH_DIRS 16             /* PUSHD's stack */
#define BATCH_ARGS 10             /* %0 .. %9 (more arguments are kept for SHIFT and %*) */
#define BATCH_NUL (-2)            /* the stream of the NUL device: writing to it does nothing */

enum { BATCH_IN = 0, BATCH_OUT = 1, BATCH_ERR = 2 };
enum { BATCH_KIND_NONE = 0, BATCH_KIND_FILE = 1, BATCH_KIND_FOLDER = 2 };
enum { BATCH_READ = 0, BATCH_WRITE = 1, BATCH_APPEND = 2 };

/* A program to start (the host makes the process). */
typedef struct BatchRun {
  const WCHAR *program;           /* its full path */
  const WCHAR *command_line;      /* what the program gets: its path in quotes, then its arguments */
  const WCHAR *directory;         /* where it starts */
  int wait;                       /* wait for it to end and give its exit code */
  int input, output, error;       /* its standard streams: BATCH_IN/OUT/ERR, BATCH_NUL or a stream of open_stream */
} BatchRun;

typedef struct BatchHost {
  void *context;
  /* A block of `bytes` bytes, zeroed, or NULL; given back with release. */
  void *(*alloc)(void *context, unsigned long bytes);
  void (*release)(void *context, void *block);
  /* The value of an environment variable into `out` (ended by a 0): its length, or -1 if it is not set or does not fit. */
  int (*get_variable)(void *context, const WCHAR *name, WCHAR *out, int capacity);
  /* Sets (`value`) or removes (NULL) a variable. 0 if that failed. */
  int (*set_variable)(void *context, const WCHAR *name, const WCHAR *value);
  /* The current folder ("C:\dir"), its length; 0 on failure. */
  int (*current_directory)(void *context, WCHAR *out, int capacity);
  /* Makes the folder `path` (a full path) the current one. 0 if it could not. */
  int (*change_directory)(void *context, const WCHAR *path);
  /* `path` made full (against the current folder, "." and ".." applied), its length; 0 on failure. */
  int (*full_path)(void *context, const WCHAR *path, WCHAR *out, int capacity);
  /* What `path` is: BATCH_KIND_NONE, _FILE or _FOLDER. */
  int (*file_kind)(void *context, const WCHAR *path);
  /* Starts a program; 0 if it could not be started. With run->wait, *exit_code is what it ended with. */
  int (*run)(void *context, const BatchRun *run, unsigned *exit_code);
  /* Writes to a stream (BATCH_OUT, BATCH_ERR or one of open_stream). */
  void (*write)(void *context, int stream, const WCHAR *text, int length);
  /* Opens a file for redirection (mode BATCH_READ, _WRITE, _APPEND): a stream number above 2, or -1. */
  int (*open_stream)(void *context, const WCHAR *path, int mode);
  void (*close_stream)(void *context, int stream);
  /* The text of a batch file, in a block of alloc that the interpreter releases; 0 if it could not be read. */
  int (*load_script)(void *context, const WCHAR *path, WCHAR **text, int *length);
  /* Reads a line from the standard input (without its end), its length, or -1 at the end of the input. */
  int (*read_line)(void *context, WCHAR *out, int capacity);
  /* Copies a file's bytes to a stream (TYPE). 0 if it could not be read. */
  int (*type_file)(void *context, const WCHAR *path, int stream);
} BatchHost;

/* --- text ------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct { WCHAR *data; int capacity; int length; int overflow; } BText;      /* capacity counts the ending 0 */
typedef struct { const WCHAR *p; int n; } BView;

static void bt_init(BText *t, WCHAR *storage, int capacity) { t->data = storage; t->capacity = capacity; t->length = 0; t->overflow = 0; storage[0] = 0; }
static void bt_put(BText *t, WCHAR c) {
  if (t->length >= t->capacity - 1) { t->overflow = 1; return; }
  t->data[t->length++] = c;
  t->data[t->length] = 0;
}
static void bt_view(BText *t, BView v) { for (int i = 0; i < v.n; ++i) bt_put(t, v.p[i]); }
static void bt_wstr(BText *t, const WCHAR *s) { while (*s) bt_put(t, *s++); }
static void bt_ascii(BText *t, const char *s) { while (*s) bt_put(t, (WCHAR)(unsigned char)*s++); }
static void bt_number(BText *t, unsigned value) {
  char digits[12];
  int n = 0;
  do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < 11);
  while (n) bt_put(t, (WCHAR)digits[--n]);
}

static BView bv(const WCHAR *p, int n) { BView v; v.p = p; v.n = n; return v; }
static int bw_len(const WCHAR *s) { int n = 0; while (s[n]) ++n; return n; }
static BView bv_str(const WCHAR *s) { return bv(s, bw_len(s)); }
static WCHAR b_lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
static int b_blank(WCHAR c) { return c == ' ' || c == '\t'; }
/* The characters that end a word of a command line (cmd's delimiters). */
static int b_delimiter(WCHAR c) { return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '='; }
static BView bv_trim(BView v) {
  while (v.n > 0 && b_blank(v.p[0])) { ++v.p; --v.n; }
  while (v.n > 0 && b_blank(v.p[v.n - 1])) --v.n;
  return v;
}
static BView bv_skip_blanks(BView v) { while (v.n > 0 && b_blank(v.p[0])) { ++v.p; --v.n; } return v; }
static int bv_equals_ascii(BView v, const char *s) {      /* (case-insensitive) */
  int i = 0;
  for (; s[i]; ++i) if (i >= v.n || b_lower(v.p[i]) != b_lower((WCHAR)(unsigned char)s[i])) return 0;
  return i == v.n;
}
static int bv_starts_ascii(BView v, const char *s) {
  for (int i = 0; s[i]; ++i) if (i >= v.n || b_lower(v.p[i]) != b_lower((WCHAR)(unsigned char)s[i])) return 0;
  return 1;
}
static int bv_equal(BView a, BView b, int ignore_case) {
  if (a.n != b.n) return 0;
  for (int i = 0; i < a.n; ++i) if (ignore_case ? b_lower(a.p[i]) != b_lower(b.p[i]) : a.p[i] != b.p[i]) return 0;
  return 1;
}
static BView bv_unquote(BView v) {
  v = bv_trim(v);
  if (v.n >= 1 && v.p[0] == '"') { ++v.p; --v.n; if (v.n >= 1 && v.p[v.n - 1] == '"') --v.n; }
  return v;
}
static int bv_ends_ascii(BView v, const char *s) {
  int n = 0;
  while (s[n]) ++n;
  return v.n >= n && bv_equals_ascii(bv(v.p + v.n - n, n), s);
}
/* A view copied into a buffer with an ending 0; 0 if it does not fit. */
static int bv_copy(BView v, WCHAR *out, int capacity) {
  if (v.n >= capacity) return 0;
  for (int i = 0; i < v.n; ++i) out[i] = v.p[i];
  out[v.n] = 0;
  return 1;
}
/* The next word: blanks and delimiters before it skipped; a word runs to a delimiter outside quotes (the quotes stay in it). */
static BView bv_next_word(BView *rest) {
  BView r = *rest;
  while (r.n > 0 && b_delimiter(r.p[0])) { ++r.p; --r.n; }
  int i = 0, quoted = 0;
  for (; i < r.n; ++i) {
    if (r.p[i] == '"') quoted = !quoted;
    else if (!quoted && b_delimiter(r.p[i])) break;
  }
  *rest = bv(r.p + i, r.n - i);
  return bv(r.p, i);
}

/* --- the interpreter's state ----------------------------------------------------------------------------------------------------------------- */

/* A batch file (or a :label CALLed in one) that runs. */
typedef struct BatchFrame {
  const WCHAR *text;              /* the file's text; NULL for a single command line */
  int length, position;           /* where the next line starts */
  WCHAR *owned;                   /* the text, when this frame loaded it */
  WCHAR script[BATCH_PATH];       /* the batch file's full path (what %~dp0 says, in a :label too) */
  WCHAR arguments[BATCH_LINE + 1];      /* %0, %1, ... one after the other, each ended by a 0 */
  int argument_start[64], argument_count, shift;
  WCHAR line[BATCH_LINE + 1], expanded[BATCH_LINE + 1], segment[BATCH_LINE + 1], scratch[BATCH_LINE + 1];
  int done;                       /* EXIT /B, GOTO :EOF or the end of the text */
} BatchFrame;

typedef struct BatchShell {
  const BatchHost *host;
  int echo;                       /* ECHO ON: each command is shown, after the prompt, before it runs */
  unsigned errorlevel;
  int exit_all;                   /* EXIT without /B: cmd ends */
  unsigned exit_code;
  int depth;
  int out, err, in;               /* the streams of the command that runs (redirected or not) */
  int last_ok;                    /* did the last command succeed (for && and ||) */
  WCHAR dirs[BATCH_DIRS][BATCH_PATH];
  int dir_count;
} BatchShell;

static void batch_shell_init(BatchShell *shell, const BatchHost *host) {
  shell->host = host;
  shell->echo = 1;
  shell->errorlevel = 0;
  shell->exit_all = 0;
  shell->exit_code = 0;
  shell->depth = 0;
  shell->out = BATCH_OUT;
  shell->err = BATCH_ERR;
  shell->in = BATCH_IN;
  shell->last_ok = 1;
  shell->dir_count = 0;
}

static void batch_write(BatchShell *shell, int stream, BView text) {
  if (stream == BATCH_NUL || text.n == 0) return;
  shell->host->write(shell->host->context, stream, text.p, text.n);
}
static void batch_write_ascii(BatchShell *shell, int stream, const char *text) {
  WCHAR buffer[256];
  int n = 0;
  while (text[n] && n < 255) { buffer[n] = (WCHAR)(unsigned char)text[n]; ++n; }
  batch_write(shell, stream, bv(buffer, n));
}
/* A message ending in a line break, to the error stream. */
static void batch_error(BatchShell *shell, const char *message) {
  batch_write_ascii(shell, shell->err, message);
  batch_write_ascii(shell, shell->err, "\r\n");
}
static void batch_error_with(BatchShell *shell, const char *before, BView what, const char *after) {
  batch_write_ascii(shell, shell->err, before);
  batch_write(shell, shell->err, what);
  batch_write_ascii(shell, shell->err, after);
  batch_write_ascii(shell, shell->err, "\r\n");
}

static int batch_cwd(BatchShell *shell, WCHAR *out, int capacity) {
  const int n = shell->host->current_directory(shell->host->context, out, capacity);
  if (n <= 0 || n >= capacity) { out[0] = 0; return 0; }
  return n;
}

/* --- arguments --------------------------------------------------------------------------------------------------------------------------- */

/* The frame's %0 (`name`) and the words of `rest` as %1, %2, ... 0 if they do not fit. */
static int batch_set_arguments(BatchFrame *frame, BView name, BView rest) {
  BText t;
  bt_init(&t, frame->arguments, BATCH_LINE + 1);
  frame->argument_count = 0;
  frame->shift = 0;
  BView word = name;
  for (;;) {
    if (frame->argument_count >= 64) break;
    frame->argument_start[frame->argument_count++] = t.length;
    bt_view(&t, word);
    bt_put(&t, 0);
    word = bv_next_word(&rest);
    if (word.n == 0) break;
  }
  return !t.overflow;
}
/* %N (after SHIFT); empty when there is no such argument. */
static BView batch_argument(const BatchFrame *frame, int index) {
  const int at = index == 0 ? 0 : index + frame->shift;
  if (index < 0 || at >= frame->argument_count) return bv(frame->arguments, 0);
  const WCHAR *p = frame->arguments + frame->argument_start[at];
  return bv_str(p);
}

/* The parts of a path (%~dpnx): the full path is worked out first. `modifiers` holds the letters. */
static void batch_path_parts(BatchShell *shell, BText *out, BView path, BView modifiers) {
  WCHAR copy[BATCH_PATH], full[BATCH_PATH];
  path = bv_unquote(path);
  int f = 0, d = 0, p = 0, n = 0, x = 0;
  for (int i = 0; i < modifiers.n; ++i) {
    const WCHAR c = b_lower(modifiers.p[i]);
    f |= c == 'f'; d |= c == 'd'; p |= c == 'p'; n |= c == 'n'; x |= c == 'x';
  }
  if (!f && !d && !p && !n && !x) { bt_view(out, path); return; }      /* %~1: without its quotes */
  if (path.n == 0) return;
  if (!bv_copy(path, copy, BATCH_PATH)) return;
  const int length = shell->host->full_path(shell->host->context, copy, full, BATCH_PATH);
  if (length <= 0 || length >= BATCH_PATH) return;
  const BView whole = bv(full, length);
  if (f && !d && !p && !n && !x) { bt_view(out, whole); return; }
  if (f) d = p = n = x = 1;
  int drive = (length >= 2 && full[1] == ':') ? 2 : 0;
  int slash = -1;
  for (int i = 0; i < length; ++i) if (full[i] == '\\' || full[i] == '/') slash = i;
  const int name_start = slash + 1;
  int dot = -1;
  for (int i = name_start; i < length; ++i) if (full[i] == '.') dot = i;
  const int name_end = dot < 0 ? length : dot;
  if (drive > name_start) drive = 0;
  if (d) bt_view(out, bv(full, drive));
  if (p) bt_view(out, bv(full + drive, name_start - drive));
  if (n) bt_view(out, bv(full + name_start, name_end - name_start));
  if (x) bt_view(out, bv(full + name_end, length - name_end));
}

/* --- expansion (%...%) ------------------------------------------------------------------------------------------------------------------- */

/* The value of a variable, the dynamic ones included; -1 when it is not set. */
static int batch_variable(BatchShell *shell, BView name, WCHAR *out, int capacity) {
  WCHAR key[256];
  if (name.n == 0 || !bv_copy(name, key, 256)) return -1;
  const int n = shell->host->get_variable(shell->host->context, key, out, capacity);
  if (n >= 0) return n;
  if (bv_equals_ascii(name, "CD")) return batch_cwd(shell, out, capacity);
  if (bv_equals_ascii(name, "ERRORLEVEL")) {
    BText t;
    bt_init(&t, out, capacity);
    bt_number(&t, shell->errorlevel);
    return t.overflow ? -1 : t.length;
  }
  return -1;
}

/* A number at the start of `v` (with a sign); how many characters it took in *used. */
static int batch_parse_int(BView v, int *used) {
  int i = 0, sign = 1, value = 0;
  if (i < v.n && (v.p[i] == '-' || v.p[i] == '+')) { sign = v.p[i] == '-' ? -1 : 1; ++i; }
  const int digits_at = i;
  while (i < v.n && v.p[i] >= '0' && v.p[i] <= '9') { if (value < 100000000) value = value * 10 + (v.p[i] - '0'); ++i; }
  *used = i == digits_at ? 0 : i;
  return sign * value;
}

/* %NAME:~start,length% and %NAME:old=new% on `value`. */
static void batch_edit_value(BText *out, BView value, BView edit) {
  if (edit.n >= 1 && edit.p[0] == '~') {
    BView rest = bv(edit.p + 1, edit.n - 1);
    int used = 0;
    int start = batch_parse_int(rest, &used);
    int length = value.n;
    int have_length = 0;
    if (used < rest.n && rest.p[used] == ',') {
      BView l = bv(rest.p + used + 1, rest.n - used - 1);
      int used2 = 0;
      length = batch_parse_int(l, &used2);
      have_length = 1;
    }
    if (start < 0) start = value.n + start;
    if (start < 0) start = 0;
    if (start > value.n) start = value.n;
    int end = value.n;
    if (have_length) end = length < 0 ? value.n + length : start + (length > value.n - start ? value.n - start : length);
    if (end > value.n) end = value.n;
    if (end > start) bt_view(out, bv(value.p + start, end - start));
    return;
  }
  int equals = -1;
  for (int i = 0; i < edit.n; ++i) if (edit.p[i] == '=') { equals = i; break; }
  if (equals < 0) { bt_view(out, value); return; }
  BView from = bv(edit.p, equals), to = bv(edit.p + equals + 1, edit.n - equals - 1);
  int leading = 0;      /* "*old=new": everything up to the first old is replaced */
  if (from.n >= 1 && from.p[0] == '*') { leading = 1; ++from.p; --from.n; }
  if (from.n == 0) { bt_view(out, value); return; }
  const int start = out->length;      /* (where this value begins in the line) */
  int i = 0;
  int replaced = 0;
  while (i < value.n) {
    if (i + from.n <= value.n && bv_equal(bv(value.p + i, from.n), from, 1) && !(leading && replaced)) {
      if (leading) { out->length = start; out->data[start] = 0; }
      bt_view(out, to);
      i += from.n;
      replaced = 1;
      continue;
    }
    bt_put(out, value.p[i++]);
  }
}

/* Expands the %...% of `line` into `out`. In a batch file (frame->text) an unknown variable is nothing and a lone % is dropped; on a command line
   both stay as they are, as in cmd. 0 if the result is too long. */
static int batch_expand(BatchShell *shell, BatchFrame *frame, BView line, BText *out) {
  const int batch = frame->text != NULL;
  WCHAR value[BATCH_LINE + 1];
  int i = 0;
  while (i < line.n) {
    const WCHAR c = line.p[i];
    if (c != '%') { bt_put(out, c); ++i; continue; }
    if (batch && i + 1 < line.n) {
      const WCHAR next = line.p[i + 1];
      if (next == '%') { bt_put(out, '%'); i += 2; continue; }
      if (next >= '0' && next <= '9') { bt_view(out, batch_argument(frame, next - '0')); i += 2; continue; }
      if (next == '*') {
        for (int a = 1; a + frame->shift < frame->argument_count; ++a) { if (a > 1) bt_put(out, ' '); bt_view(out, batch_argument(frame, a)); }
        i += 2;
        continue;
      }
      if (next == '~') {      /* %~[modifiers]digit */
        int j = i + 2;
        while (j < line.n && ((line.p[j] >= 'a' && line.p[j] <= 'z') || (line.p[j] >= 'A' && line.p[j] <= 'Z'))) ++j;
        if (j < line.n && line.p[j] >= '0' && line.p[j] <= '9') {
          const int index = line.p[j] - '0';
          BView argument = batch_argument(frame, index);
          if (index == 0 && frame->script[0] && (argument.n == 0 || argument.p[0] == ':')) argument = bv_str(frame->script);
          batch_path_parts(shell, out, argument, bv(line.p + i + 2, j - (i + 2)));
          i = j + 1;
          continue;
        }
      }
    }
    int close = -1;
    for (int j = i + 1; j < line.n; ++j) if (line.p[j] == '%') { close = j; break; }
    if (close < 0) {      /* no closing %: dropped in a batch file, kept on a command line */
      if (!batch) bt_put(out, '%');
      ++i;
      continue;
    }
    BView inside = bv(line.p + i + 1, close - i - 1);
    int colon = -1;
    for (int j = 0; j < inside.n; ++j) if (inside.p[j] == ':') { colon = j; break; }
    const BView name = colon < 0 ? inside : bv(inside.p, colon);
    const int length = name.n > 0 ? batch_variable(shell, name, value, BATCH_LINE + 1) : -1;
    if (length < 0) {
      if (!batch) { bt_put(out, '%'); ++i; continue; }      /* (what follows may begin a real one) */
      if (name.n == 0) { ++i; continue; }
      i = close + 1;
      continue;
    }
    if (colon < 0) bt_view(out, bv(value, length));
    else batch_edit_value(out, bv(value, length), bv(inside.p + colon + 1, inside.n - colon - 1));
    i = close + 1;
  }
  return !out->overflow;
}

/* --- finding programs -------------------------------------------------------------------------------------------------------------------- */

static int batch_is_script(BView path) { return bv_ends_ascii(path, ".bat") || bv_ends_ascii(path, ".cmd"); }

static int batch_try_file(BatchShell *shell, BView folder, BView name, BView extension, WCHAR *out) {
  WCHAR candidate[BATCH_PATH];
  BText t;
  bt_init(&t, candidate, BATCH_PATH);
  if (folder.n > 0) {
    bt_view(&t, folder);
    if (folder.p[folder.n - 1] != '\\' && folder.p[folder.n - 1] != '/') bt_put(&t, '\\');
  }
  bt_view(&t, name);
  for (int i = 0; i < extension.n; ++i) bt_put(&t, b_lower(extension.p[i]));      /* (PATHEXT is upper case; the files mostly are not) */
  if (t.overflow) return 0;
  const int n = shell->host->full_path(shell->host->context, candidate, out, BATCH_PATH);
  return n > 0 && n < BATCH_PATH && shell->host->file_kind(shell->host->context, out) == BATCH_KIND_FILE;
}

/* The file a command names: as it is when it has an extension, else with each of PATHEXT's; in the current folder, then in each of PATH's (only
   where the name has no folder in it). The full path goes into `out`. */
static int batch_find_program(BatchShell *shell, BView name, WCHAR *out) {
  WCHAR path_ext[512], path_list[BATCH_LINE + 1];
  name = bv_unquote(name);
  if (name.n == 0) return 0;
  int has_folder = 0, dot = -1;
  for (int i = 0; i < name.n; ++i) {
    if (name.p[i] == '\\' || name.p[i] == '/' || name.p[i] == ':') { has_folder = 1; dot = -1; }
    else if (name.p[i] == '.') dot = i;
  }
  int ext_length = shell->host->get_variable(shell->host->context, (const WCHAR *)u"PATHEXT", path_ext, 512);
  if (ext_length <= 0) {
    BText t;
    bt_init(&t, path_ext, 512);
    bt_ascii(&t, ".COM;.EXE;.BAT;.CMD");
    ext_length = t.length;
  }
  int path_length = has_folder ? -1 : shell->host->get_variable(shell->host->context, (const WCHAR *)u"PATH", path_list, BATCH_LINE + 1);
  /* the folders: the current one ("" = as the name says), then PATH's */
  int at = 0;
  BView folder = bv(path_list, 0);
  for (int round = 0;; ++round) {
    if (round > 0) {
      if (path_length <= 0) return 0;
      while (at < path_length && path_list[at] == ';') ++at;
      if (at >= path_length) return 0;
      int end = at;
      while (end < path_length && path_list[end] != ';') ++end;
      folder = bv_unquote(bv(path_list + at, end - at));
      at = end;
      if (folder.n == 0) continue;
    }
    if (dot >= 0 && batch_try_file(shell, folder, name, bv(path_ext, 0), out)) return 1;
    int e = 0;
    while (e < ext_length) {
      int end = e;
      while (end < ext_length && path_ext[end] != ';') ++end;
      if (end > e && batch_try_file(shell, folder, name, bv(path_ext + e, end - e), out)) return 1;
      e = end + 1;
    }
  }
}

/* --- running a command ------------------------------------------------------------------------------------------------------------------- */

static void batch_run_frame(BatchShell *shell, BatchFrame *frame);
static void batch_execute(BatchShell *shell, BatchFrame *frame, BView command);

/* Runs a batch file (or, with `label`, the :label of the frame's text) in a frame of its own, with `name` as %0 and `rest` as its arguments. */
static void batch_call(BatchShell *shell, BatchFrame *caller, const WCHAR *script_path, BView name, BView rest, BView label) {
  if (shell->depth >= BATCH_DEPTH) { batch_error(shell, "Batch files are called inside each other too deeply."); shell->errorlevel = 1; shell->last_ok = 0; return; }
  const BatchHost *host = shell->host;
  BatchFrame *frame = (BatchFrame *)host->alloc(host->context, sizeof(BatchFrame));
  if (!frame) { batch_error(shell, "Not enough memory."); shell->errorlevel = 1; shell->last_ok = 0; return; }
  frame->text = NULL;
  frame->owned = NULL;
  if (label.n > 0) {
    frame->text = caller->text;
    frame->length = caller->length;
    bv_copy(bv_str(caller->script), frame->script, BATCH_PATH);
  } else {
    WCHAR *text = NULL;
    int length = 0;
    if (!host->load_script(host->context, script_path, &text, &length) || !text) {
      batch_error_with(shell, "The batch file ", name, " cannot be read.");
      host->release(host->context, frame);
      shell->errorlevel = 1;
      shell->last_ok = 0;
      return;
    }
    frame->text = frame->owned = text;
    frame->length = length;
    bv_copy(bv_str(script_path), frame->script, BATCH_PATH);
  }
  frame->position = 0;
  frame->done = 0;
  if (!batch_set_arguments(frame, name, rest)) batch_error(shell, "The arguments are too long.");
  int found = 1;
  if (label.n > 0) {      /* (the search for a label is GOTO's) */
    found = 0;
    int at = 0;
    while (at < frame->length) {
      int end = at;
      while (end < frame->length && frame->text[end] != '\n') ++end;
      BView l = bv_skip_blanks(bv(frame->text + at, end - at));
      if (l.n > 0 && l.p[0] == ':') {
        BView rest_of_label = bv(l.p + 1, l.n - 1);
        BView word = bv_next_word(&rest_of_label);
        while (word.n > 0 && word.p[word.n - 1] == '\r') --word.n;
        if (bv_equal(word, label, 1)) { frame->position = end < frame->length ? end + 1 : end; found = 1; break; }
      }
      at = end + 1;
    }
    if (!found) batch_error_with(shell, "The system cannot find the batch label specified - ", label, "");
  }
  if (found) {
    ++shell->depth;
    batch_run_frame(shell, frame);
    --shell->depth;
  } else {
    shell->errorlevel = 1;
    shell->last_ok = 0;
  }
  if (frame->owned) host->release(host->context, frame->owned);
  host->release(host->context, frame);
}

/* Starts (or runs and waits for) the program `name` with the arguments `rest`. */
static int batch_start_program(BatchShell *shell, BatchFrame *frame, const WCHAR *program, BView rest, const WCHAR *directory, int wait, unsigned *exit_code) {
  BText line;
  bt_init(&line, frame->scratch, BATCH_LINE + 1);
  WCHAR comspec[BATCH_PATH];
  const WCHAR *image = program;
  if (batch_is_script(bv_str(program))) {      /* a batch file started on its own: a new cmd /c runs it */
    if (shell->host->get_variable(shell->host->context, (const WCHAR *)u"ComSpec", comspec, BATCH_PATH) <= 0) {
      BText t;
      bt_init(&t, comspec, BATCH_PATH);
      bt_ascii(&t, "C:\\Windows\\System32\\cmd.exe");
    }
    image = comspec;
    bt_put(&line, '"'); bt_wstr(&line, comspec); bt_ascii(&line, "\" /c \"\"");
    bt_wstr(&line, program);
    bt_put(&line, '"');
    if (rest.n > 0) { bt_put(&line, ' '); bt_view(&line, rest); }
    bt_put(&line, '"');
  } else {
    bt_put(&line, '"'); bt_wstr(&line, program); bt_put(&line, '"');
    if (rest.n > 0) { bt_put(&line, ' '); bt_view(&line, rest); }
  }
  if (line.overflow) { batch_error(shell, "The command line is too long."); return 0; }
  WCHAR cwd[BATCH_PATH];
  if (!directory) { batch_cwd(shell, cwd, BATCH_PATH); directory = cwd; }
  BatchRun run;
  run.program = image;
  run.command_line = line.data;
  run.directory = directory;
  run.wait = wait;
  run.input = shell->in;
  run.output = shell->out;
  run.error = shell->err;
  *exit_code = 0;
  return shell->host->run(shell->host->context, &run, exit_code);
}

static void batch_not_found(BatchShell *shell, BView name) {
  batch_error_with(shell, "'", bv_unquote(name), "' is not recognized as an internal or external command,");
  batch_error(shell, "operable program or batch file.");
  shell->errorlevel = 9009;
  shell->last_ok = 0;
}

/* A program or a batch file named as a command. */
static void batch_command_program(BatchShell *shell, BatchFrame *frame, BView name, BView rest) {
  WCHAR program[BATCH_PATH];
  if (!batch_find_program(shell, name, program)) { batch_not_found(shell, name); return; }
  if (batch_is_script(bv_str(program))) {      /* takes over from the batch file that runs: that one does not go on */
    batch_call(shell, frame, program, name, rest, bv(program, 0));
    if (frame->text) frame->done = 1;
    return;
  }
  unsigned code = 0;
  if (!batch_start_program(shell, frame, program, bv_trim(rest), NULL, 1, &code)) {
    batch_error_with(shell, "The program ", bv_str(program), " could not be started.");
    shell->errorlevel = 9009;
    shell->last_ok = 0;
    return;
  }
  shell->errorlevel = code;
  shell->last_ok = code == 0;
}

/* CD: no argument shows the current folder; "/D" is accepted (there is one drive). Slashes are taken for backslashes and doubled ones as one, so
   that "%~dp0/bin" works as in cmd. */
static void batch_command_cd(BatchShell *shell, BView rest) {
  WCHAR wanted[BATCH_PATH], full[BATCH_PATH];
  rest = bv_trim(rest);
  if (rest.n >= 2 && rest.p[0] == '/' && b_lower(rest.p[1]) == 'd' && (rest.n == 2 || b_blank(rest.p[2]))) rest = bv_trim(bv(rest.p + 2, rest.n - 2));
  if (rest.n == 0) {
    const int n = batch_cwd(shell, full, BATCH_PATH);
    batch_write(shell, shell->out, bv(full, n));
    batch_write_ascii(shell, shell->out, "\r\n");
    shell->last_ok = 1;
    return;
  }
  /* (the whole rest is the folder, spaces and all; quotes are taken away wherever they are) */
  BText t;
  bt_init(&t, wanted, BATCH_PATH);
  for (int i = 0; i < rest.n; ++i) {
    WCHAR c = rest.p[i];
    if (c == '"') continue;
    if (c == '/') c = '\\';
    if (c == '\\' && t.length > 1 && t.data[t.length - 1] == '\\') continue;
    bt_put(&t, c);
  }
  while (t.length > 0 && b_blank(t.data[t.length - 1])) t.data[--t.length] = 0;
  const int n = t.overflow ? 0 : shell->host->full_path(shell->host->context, wanted, full, BATCH_PATH);
  if (n <= 0 || n >= BATCH_PATH || shell->host->file_kind(shell->host->context, full) != BATCH_KIND_FOLDER ||
      !shell->host->change_directory(shell->host->context, full)) {
    batch_error(shell, "The system cannot find the path specified.");
    shell->errorlevel = 1;
    shell->last_ok = 0;
    return;
  }
  shell->last_ok = 1;
}

/* ECHO: ON/OFF, the state, or the text after the first character (a blank, or "." "/" ":" ... for an empty line). */
static void batch_command_echo(BatchShell *shell, BView rest) {
  shell->last_ok = 1;
  const BView trimmed = bv_trim(rest);
  if (rest.n == 0 || (b_blank(rest.p[0]) && trimmed.n == 0)) {
    batch_write_ascii(shell, shell->out, shell->echo ? "ECHO is on.\r\n" : "ECHO is off.\r\n");
    return;
  }
  if (b_blank(rest.p[0]) && bv_equals_ascii(trimmed, "on")) { shell->echo = 1; return; }
  if (b_blank(rest.p[0]) && bv_equals_ascii(trimmed, "off")) { shell->echo = 0; return; }
  BView text = bv(rest.p + 1, rest.n - 1);
  while (text.n > 0 && b_blank(text.p[text.n - 1])) --text.n;      /* (what a redirection left behind) */
  batch_write(shell, shell->out, text);
  batch_write_ascii(shell, shell->out, "\r\n");
}

/* SET name=value, SET "name=value", SET name (shows it). */
static void batch_command_set(BatchShell *shell, BView rest) {
  WCHAR name[256], value[BATCH_LINE + 1];
  rest = bv_trim(rest);
  if (rest.n >= 2 && rest.p[0] == '/') { batch_error(shell, "SET /A and SET /P are not supported."); shell->errorlevel = 1; shell->last_ok = 0; return; }
  if (rest.n == 0) { batch_error(shell, "SET without a name is not supported."); shell->last_ok = 0; return; }
  if (rest.p[0] == '"') {      /* "name=value" with whatever follows the last quote dropped */
    int last = rest.n - 1;
    while (last > 0 && rest.p[last] != '"') --last;
    rest = last > 0 ? bv(rest.p + 1, last - 1) : bv(rest.p + 1, rest.n - 1);
  }
  int equals = -1;
  for (int i = 0; i < rest.n; ++i) if (rest.p[i] == '=') { equals = i; break; }
  if (equals < 0) {
    const int n = bv_copy(rest, name, 256) ? shell->host->get_variable(shell->host->context, name, value, BATCH_LINE + 1) : -1;
    if (n < 0) { batch_error_with(shell, "Environment variable ", rest, " not defined"); shell->errorlevel = 1; shell->last_ok = 0; return; }
    batch_write(shell, shell->out, rest);
    batch_write_ascii(shell, shell->out, "=");
    batch_write(shell, shell->out, bv(value, n));
    batch_write_ascii(shell, shell->out, "\r\n");
    shell->last_ok = 1;
    return;
  }
  if (equals == 0 || !bv_copy(bv(rest.p, equals), name, 256) || !bv_copy(bv(rest.p + equals + 1, rest.n - equals - 1), value, BATCH_LINE + 1)) {
    batch_error(shell, "The syntax of the command is incorrect.");
    shell->errorlevel = 1;
    shell->last_ok = 0;
    return;
  }
  shell->last_ok = shell->host->set_variable(shell->host->context, name, value[0] ? value : NULL) != 0 || !value[0];
}

/* START ["title"] [/D folder] [/WAIT] [/B] [/MIN] [/MAX] ... program [arguments]: the program runs beside the batch file (with /WAIT, it is
   waited for). */
static void batch_command_start(BatchShell *shell, BatchFrame *frame, BView rest) {
  WCHAR directory[BATCH_PATH], program[BATCH_PATH];
  int wait = 0, have_directory = 0;
  rest = bv_skip_blanks(rest);
  if (rest.n > 0 && rest.p[0] == '"') {      /* the first quoted word is the window's title */
    int end = 1;
    while (end < rest.n && rest.p[end] != '"') ++end;
    rest = bv_skip_blanks(bv(rest.p + end + (end < rest.n), rest.n - end - (end < rest.n)));
  }
  for (;;) {
    if (rest.n < 2 || rest.p[0] != '/') break;
    BView option = bv_next_word(&rest);
    rest = bv_skip_blanks(rest);
    if (bv_equals_ascii(option, "/wait")) wait = 1;
    else if (bv_starts_ascii(option, "/d")) {      /* "/Dfolder" or "/D folder" */
      BView folder = option.n > 2 ? bv(option.p + 2, option.n - 2) : bv_next_word(&rest);
      rest = bv_skip_blanks(rest);
      WCHAR wanted[BATCH_PATH];
      if (!bv_copy(bv_unquote(folder), wanted, BATCH_PATH)) { batch_error(shell, "The folder name is too long."); shell->last_ok = 0; return; }
      const int n = shell->host->full_path(shell->host->context, wanted, directory, BATCH_PATH);
      have_directory = n > 0 && n < BATCH_PATH;
    }
    /* (/B /I /MIN /MAX /NORMAL /LOW /HIGH /REALTIME /ABOVENORMAL /BELOWNORMAL /AFFINITY /NODE /SEPARATE /SHARED: nothing to do here) */
  }
  BView name = bv_next_word(&rest);
  if (name.n == 0) { batch_error(shell, "START without a program is not supported (there is no new console window)."); shell->last_ok = 0; return; }
  int found = 0;
  if (have_directory) {      /* (a program named in the folder of /D is found there) */
    WCHAR saved[BATCH_PATH];
    if (batch_cwd(shell, saved, BATCH_PATH) && shell->host->change_directory(shell->host->context, directory)) {
      found = batch_find_program(shell, name, program);
      shell->host->change_directory(shell->host->context, saved);
    }
  } else {
    found = batch_find_program(shell, name, program);
  }
  if (!found) {
    batch_error_with(shell, "The system cannot find the file ", bv_unquote(name), ".");
    shell->errorlevel = 9059;
    shell->last_ok = 0;
    return;
  }
  unsigned code = 0;
  if (!batch_start_program(shell, frame, program, bv_trim(rest), have_directory ? directory : NULL, wait, &code)) {
    batch_error_with(shell, "The program ", bv_str(program), " could not be started.");
    shell->errorlevel = 9059;
    shell->last_ok = 0;
    return;
  }
  shell->errorlevel = wait ? code : 0;
  shell->last_ok = !wait || code == 0;
}

/* IF [/I] [NOT] (ERRORLEVEL n | EXIST path | DEFINED name | a==b) command */
static void batch_command_if(BatchShell *shell, BatchFrame *frame, BView rest) {
  int ignore_case = 0, negate = 0;
  BView word = bv_next_word(&rest);
  if (bv_equals_ascii(word, "/i")) { ignore_case = 1; word = bv_next_word(&rest); }
  if (bv_equals_ascii(word, "not")) { negate = 1; word = bv_next_word(&rest); }
  int result = 0;
  if (bv_equals_ascii(word, "errorlevel")) {
    BView number = bv_next_word(&rest);
    int used = 0;
    const int level = batch_parse_int(number, &used);
    result = (int)shell->errorlevel >= level;
  } else if (bv_equals_ascii(word, "exist")) {
    WCHAR wanted[BATCH_PATH], full[BATCH_PATH];
    BView path = bv_unquote(bv_next_word(&rest));
    const int n = bv_copy(path, wanted, BATCH_PATH) ? shell->host->full_path(shell->host->context, wanted, full, BATCH_PATH) : 0;
    result = n > 0 && n < BATCH_PATH && shell->host->file_kind(shell->host->context, full) != BATCH_KIND_NONE;
  } else if (bv_equals_ascii(word, "defined")) {
    WCHAR value[BATCH_LINE + 1];
    result = batch_variable(shell, bv_next_word(&rest), value, BATCH_LINE + 1) >= 0;
  } else {      /* a==b: the comparison was not a word on its own, so find the == in what is there */
    BView all = bv_skip_blanks(bv(word.p, (int)(rest.p - word.p) + rest.n));
    int i = 0, quoted = 0, split = -1;
    for (; i + 1 < all.n; ++i) {
      if (all.p[i] == '"') quoted = !quoted;
      else if (!quoted && all.p[i] == '=' && all.p[i + 1] == '=') { split = i; break; }
    }
    if (split < 0) { batch_error(shell, "The syntax of the command is incorrect."); shell->errorlevel = 1; shell->last_ok = 0; return; }
    BView left = bv_trim(bv(all.p, split));
    BView after = bv_skip_blanks(bv(all.p + split + 2, all.n - split - 2));
    BView right = bv_next_word(&after);
    result = bv_equal(left, right, ignore_case);
    rest = after;
  }
  rest = bv_skip_blanks(rest);
  if (rest.n > 0 && rest.p[0] == '(') { batch_error(shell, "IF with a block in parentheses is not supported."); shell->errorlevel = 1; shell->last_ok = 0; return; }
  if (result != negate) batch_execute(shell, frame, rest);
  else shell->last_ok = 1;
}

/* GOTO label / GOTO :EOF: the line after the label, searched for from the top of the batch file. */
static void batch_command_goto(BatchShell *shell, BatchFrame *frame, BView rest) {
  if (!frame->text) { shell->last_ok = 0; return; }      /* (no labels on a command line) */
  BView label = bv_next_word(&rest);
  if (label.n > 0 && label.p[0] == ':') { ++label.p; --label.n; }
  if (bv_equals_ascii(label, "eof")) { frame->done = 1; return; }
  int at = 0;
  while (at < frame->length) {
    int end = at;
    while (end < frame->length && frame->text[end] != '\n') ++end;
    BView l = bv_skip_blanks(bv(frame->text + at, end - at));
    if (l.n > 1 && l.p[0] == ':' && l.p[1] != ':') {
      BView after = bv(l.p + 1, l.n - 1);
      BView word = bv_next_word(&after);
      while (word.n > 0 && word.p[word.n - 1] == '\r') --word.n;
      if (bv_equal(word, label, 1)) { frame->position = end < frame->length ? end + 1 : end; return; }
    }
    at = end + 1;
  }
  batch_error_with(shell, "The system cannot find the batch label specified - ", label, "");
  shell->errorlevel = 1;
  frame->done = 1;
}

static void batch_command_call(BatchShell *shell, BatchFrame *frame, BView rest) {
  rest = bv_skip_blanks(rest);
  BView name = bv_next_word(&rest);
  if (name.n == 0) { shell->last_ok = 1; return; }
  if (name.p[0] == ':') {
    if (!frame->text) { batch_error(shell, "Invalid attempt to call batch label outside of batch script."); shell->last_ok = 0; return; }
    batch_call(shell, frame, frame->script, name, rest, bv(name.p + 1, name.n - 1));
    return;
  }
  WCHAR program[BATCH_PATH];
  if (!batch_find_program(shell, name, program)) { batch_not_found(shell, name); return; }
  if (batch_is_script(bv_str(program))) { batch_call(shell, frame, program, name, rest, bv(program, 0)); return; }
  batch_command_program(shell, frame, name, rest);
}

static void batch_command_pushd(BatchShell *shell, BView rest) {
  WCHAR here[BATCH_PATH];
  if (shell->dir_count >= BATCH_DIRS || !batch_cwd(shell, here, BATCH_PATH)) { batch_error(shell, "Too many folders were pushed."); shell->last_ok = 0; return; }
  if (bv_trim(rest).n == 0) { shell->last_ok = 1; return; }
  batch_command_cd(shell, rest);
  if (shell->last_ok) { bv_copy(bv_str(here), shell->dirs[shell->dir_count], BATCH_PATH); ++shell->dir_count; }
}

static void batch_command_popd(BatchShell *shell) {
  if (shell->dir_count == 0) { shell->last_ok = 1; return; }
  --shell->dir_count;
  shell->last_ok = shell->host->change_directory(shell->host->context, shell->dirs[shell->dir_count]);
}

static void batch_command_exit(BatchShell *shell, BatchFrame *frame, BView rest) {
  BView word = bv_next_word(&rest);
  int local = 0;
  if (bv_equals_ascii(word, "/b")) { local = 1; word = bv_next_word(&rest); }
  if (word.n > 0) {
    int used = 0;
    shell->errorlevel = (unsigned)batch_parse_int(word, &used);
  }
  if (local && frame->text) { frame->done = 1; return; }
  shell->exit_all = 1;
  shell->exit_code = shell->errorlevel;
}

/* The built-in commands: the name, how it may be followed ("echo." "cd.." "cd\"), what runs it. */
typedef enum {
  B_NONE, B_REM, B_ECHO, B_CD, B_SET, B_START, B_CALL, B_GOTO, B_EXIT, B_IF, B_SHIFT, B_PUSHD, B_POPD, B_PAUSE, B_TYPE, B_VER, B_NOTHING, B_UNSUPPORTED
} BatchBuiltin;
static BatchBuiltin batch_builtin(BView command, BView *rest) {
  static const struct { const char *name; BatchBuiltin kind; } kTable[] = {
    {"rem", B_REM}, {"echo", B_ECHO}, {"cd", B_CD}, {"chdir", B_CD}, {"set", B_SET}, {"start", B_START}, {"call", B_CALL}, {"goto", B_GOTO},
    {"exit", B_EXIT}, {"if", B_IF}, {"shift", B_SHIFT}, {"pushd", B_PUSHD}, {"popd", B_POPD}, {"pause", B_PAUSE}, {"type", B_TYPE}, {"ver", B_VER},
    {"title", B_NOTHING}, {"cls", B_NOTHING}, {"color", B_NOTHING}, {"setlocal", B_NOTHING}, {"endlocal", B_NOTHING}, {"verify", B_NOTHING},
    {"for", B_UNSUPPORTED}, {"else", B_UNSUPPORTED}, {"choice", B_UNSUPPORTED},
  };
  for (unsigned i = 0; i < sizeof(kTable) / sizeof(kTable[0]); ++i) {
    const char *name = kTable[i].name;
    int n = 0;
    while (name[n]) ++n;
    if (!bv_starts_ascii(command, name)) continue;
    if (command.n == n) { *rest = bv(command.p + n, 0); return kTable[i].kind; }
    const WCHAR next = command.p[n];
    if (b_blank(next) || next == '.' || next == '/' || next == '\\' || next == ':' || next == '(' || next == '+' || next == ',' || next == ';' || next == '=') {
      if (kTable[i].kind == B_REM && !b_blank(next) && next != '/') continue;
      *rest = bv(command.p + n, command.n - n);
      return kTable[i].kind;
    }
  }
  return B_NONE;
}

/* One command, already expanded and without its redirections. */
static void batch_execute(BatchShell *shell, BatchFrame *frame, BView command) {
  command = bv_skip_blanks(command);
  while (command.n > 0 && (command.p[0] == '@' || b_blank(command.p[0]))) { ++command.p; --command.n; }
  while (command.n > 0 && (b_blank(command.p[command.n - 1]) || command.p[command.n - 1] == '\r')) --command.n;
  if (command.n == 0) return;
  if (command.n == 2 && command.p[1] == ':' && ((command.p[0] >= 'a' && command.p[0] <= 'z') || (command.p[0] >= 'A' && command.p[0] <= 'Z'))) {
    WCHAR root[4] = {command.p[0], ':', '\\', 0}, here[BATCH_PATH];      /* "D:": that drive's root, unless it is the current drive */
    if (batch_cwd(shell, here, BATCH_PATH) && b_lower(here[0]) == b_lower(command.p[0])) { shell->last_ok = 1; return; }
    if (shell->host->file_kind(shell->host->context, root) != BATCH_KIND_FOLDER || !shell->host->change_directory(shell->host->context, root)) {
      batch_error(shell, "The system cannot find the drive specified.");
      shell->errorlevel = 1;
      shell->last_ok = 0;
      return;
    }
    shell->last_ok = 1;
    return;
  }
  if (command.p[0] == '(') { batch_error(shell, "Blocks in parentheses are not supported."); shell->errorlevel = 1; shell->last_ok = 0; return; }
  BView rest;
  switch (batch_builtin(command, &rest)) {
    case B_REM: case B_NOTHING: shell->last_ok = 1; return;
    case B_ECHO: batch_command_echo(shell, rest); return;
    case B_CD: batch_command_cd(shell, rest); return;
    case B_SET: batch_command_set(shell, rest); return;
    case B_START: batch_command_start(shell, frame, rest); return;
    case B_CALL: batch_command_call(shell, frame, rest); return;
    case B_GOTO: batch_command_goto(shell, frame, rest); return;
    case B_EXIT: batch_command_exit(shell, frame, rest); return;
    case B_IF: batch_command_if(shell, frame, rest); return;
    case B_SHIFT: if (frame->shift + 1 < frame->argument_count) ++frame->shift; shell->last_ok = 1; return;
    case B_PUSHD: batch_command_pushd(shell, rest); return;
    case B_POPD: batch_command_popd(shell); return;
    case B_PAUSE: {
      WCHAR line[BATCH_PATH];
      batch_write_ascii(shell, shell->out, "Press any key to continue . . . ");
      shell->host->read_line(shell->host->context, line, BATCH_PATH);
      batch_write_ascii(shell, shell->out, "\r\n");
      shell->last_ok = 1;
      return;
    }
    case B_TYPE: {
      WCHAR wanted[BATCH_PATH], full[BATCH_PATH];
      BView path = bv_unquote(bv_next_word(&rest));
      const int n = bv_copy(path, wanted, BATCH_PATH) ? shell->host->full_path(shell->host->context, wanted, full, BATCH_PATH) : 0;
      if (n <= 0 || n >= BATCH_PATH || !shell->host->type_file(shell->host->context, full, shell->out)) {
        batch_error(shell, "The system cannot find the file specified.");
        shell->errorlevel = 1;
        shell->last_ok = 0;
        return;
      }
      shell->last_ok = 1;
      return;
    }
    case B_VER: batch_write_ascii(shell, shell->out, "\r\nMicrosoft Windows [Version 10.0.19045]\r\n"); shell->last_ok = 1; return;
    case B_UNSUPPORTED: {
      BView name = command;
      name = bv_next_word(&name);
      batch_error_with(shell, "The command ", name, " is not supported.");
      shell->errorlevel = 1;
      shell->last_ok = 0;
      return;
    }
    case B_NONE: break;
  }
  BView name = bv_next_word(&command);
  batch_command_program(shell, frame, name, command);
}

/* --- lines --------------------------------------------------------------------------------------------------------------------------------- */

/* The redirections of one command. */
typedef struct { int out, err, in; int err_to_out; int failed; } BatchRedirect;

static int batch_open_target(BatchShell *shell, BView target, int mode) {
  WCHAR path[BATCH_PATH];
  target = bv_unquote(target);
  if (bv_equals_ascii(target, "nul")) return BATCH_NUL;
  if (!bv_copy(target, path, BATCH_PATH)) return -1;
  return shell->host->open_stream(shell->host->context, path, mode);
}
static void batch_close_redirects(BatchShell *shell, BatchRedirect *r) {
  if (r->out > 2) shell->host->close_stream(shell->host->context, r->out);
  if (r->err > 2 && r->err != r->out) shell->host->close_stream(shell->host->context, r->err);
  if (r->in > 2) shell->host->close_stream(shell->host->context, r->in);
}

/* Runs an expanded line: its commands one after the other, as & && || say, each with its redirections. */
static void batch_run_line(BatchShell *shell, BatchFrame *frame, BView line) {
  int at = 0;
  int join = 0;      /* how this command is joined to the one before: 0 &, 1 &&, 2 || */
  while (at <= line.n && !shell->exit_all && !frame->done) {
    BText segment;
    bt_init(&segment, frame->segment, BATCH_LINE + 1);
    BatchRedirect r;
    r.out = BATCH_OUT; r.err = BATCH_ERR; r.in = BATCH_IN; r.err_to_out = 0; r.failed = 0;
    int quoted = 0, next_join = -1;
    int i = at;
    while (i < line.n) {
      const WCHAR c = line.p[i];
      if (c == '"') { quoted = !quoted; bt_put(&segment, c); ++i; continue; }
      if (quoted) { bt_put(&segment, c); ++i; continue; }
      if (c == '^' && i + 1 < line.n) { bt_put(&segment, line.p[i + 1]); i += 2; continue; }
      if (c == '&') {
        if (i + 1 < line.n && line.p[i + 1] == '&') { next_join = 1; i += 2; } else { next_join = 0; ++i; }
        break;
      }
      if (c == '|') {
        if (i + 1 < line.n && line.p[i + 1] == '|') { next_join = 2; i += 2; break; }
        batch_error(shell, "Pipes (|) are not supported.");
        shell->errorlevel = 1;
        shell->last_ok = 0;
        return;
      }
      if (c == '>' || c == '<') {
        int stream = c == '<' ? 0 : 1;
        if (c == '>' && segment.length > 0 && (segment.data[segment.length - 1] == '1' || segment.data[segment.length - 1] == '2') &&
            (segment.length == 1 || b_blank(segment.data[segment.length - 2]))) {      /* "2>": the digit was the stream's */
          stream = segment.data[segment.length - 1] - '0';
          segment.data[--segment.length] = 0;
        }
        int mode = c == '<' ? BATCH_READ : BATCH_WRITE;
        ++i;
        if (c == '>' && i < line.n && line.p[i] == '>') { mode = BATCH_APPEND; ++i; }
        if (c == '>' && i + 1 < line.n && line.p[i] == '&' && (line.p[i + 1] == '1' || line.p[i + 1] == '2')) {      /* 2>&1, 1>&2 */
          if (stream == 2 && line.p[i + 1] == '1') r.err_to_out = 1;
          else if (stream == 1 && line.p[i + 1] == '2') r.out = r.err;
          i += 2;
          continue;
        }
        while (i < line.n && b_blank(line.p[i])) ++i;
        int start = i, q = 0;
        while (i < line.n && (q || (!b_blank(line.p[i]) && line.p[i] != '&' && line.p[i] != '|' && line.p[i] != '<' && line.p[i] != '>'))) {
          if (line.p[i] == '"') q = !q;
          ++i;
        }
        const BView target = bv(line.p + start, i - start);
        if (target.n == 0) { r.failed = 1; continue; }
        const int opened = batch_open_target(shell, target, mode);
        if (opened == -1) { batch_error(shell, "The system cannot find the path specified."); r.failed = 1; continue; }
        if (stream == 0) { if (r.in > 2) shell->host->close_stream(shell->host->context, r.in); r.in = opened; }
        else if (stream == 1) { if (r.out > 2) shell->host->close_stream(shell->host->context, r.out); r.out = opened; }
        else { if (r.err > 2) shell->host->close_stream(shell->host->context, r.err); r.err = opened; }
        continue;
      }
      bt_put(&segment, c);
      ++i;
    }
    if (r.err_to_out) r.err = r.out;
    const int run = join == 0 || (join == 1 && shell->last_ok) || (join == 2 && !shell->last_ok);
    if (segment.overflow) {
      batch_error(shell, "The command line is too long.");
      shell->last_ok = 0;
    } else if (r.failed) {
      shell->errorlevel = 1;
      shell->last_ok = 0;
    } else if (run) {
      const int out = shell->out, err = shell->err, in = shell->in;
      shell->out = r.out; shell->err = r.err; shell->in = r.in;
      batch_execute(shell, frame, bv(segment.data, segment.length));
      shell->out = out; shell->err = err; shell->in = in;
    }
    batch_close_redirects(shell, &r);
    if (next_join < 0) break;
    join = next_join;
    at = i;
  }
}

/* The echo of a command (ECHO ON): a blank line, the prompt, the command. */
static void batch_echo_command(BatchShell *shell, BView line) {
  WCHAR here[BATCH_PATH];
  const int n = batch_cwd(shell, here, BATCH_PATH);
  batch_write_ascii(shell, shell->out, "\r\n");
  batch_write(shell, shell->out, bv(here, n));
  batch_write_ascii(shell, shell->out, ">");
  batch_write(shell, shell->out, line);
  batch_write_ascii(shell, shell->out, "\r\n");
}

/* One line of a batch file (or the command line): expanded, echoed if it is to be, then run. */
static void batch_line(BatchShell *shell, BatchFrame *frame, BView line) {
  while (line.n > 0 && (line.p[line.n - 1] == '\r' || line.p[line.n - 1] == '\n')) --line.n;
  const BView start = bv_skip_blanks(line);
  if (start.n == 0 || start.p[0] == ':') return;      /* empty, a label or a "::" comment */
  BText expanded;
  bt_init(&expanded, frame->expanded, BATCH_LINE + 1);
  if (!batch_expand(shell, frame, start, &expanded)) { batch_error(shell, "The command line is too long."); shell->errorlevel = 1; return; }
  const BView text = bv(expanded.data, expanded.length);
  if (frame->text && shell->echo && text.n > 0 && text.p[0] != '@') {
    BView shown = bv_trim(text);
    if (shown.n > 0) batch_echo_command(shell, shown);
  }
  batch_run_line(shell, frame, text);
}

static void batch_run_frame(BatchShell *shell, BatchFrame *frame) {
  while (!frame->done && !shell->exit_all && frame->position < frame->length) {
    int end = frame->position;
    while (end < frame->length && frame->text[end] != '\n') ++end;
    const int start = frame->position;
    frame->position = end < frame->length ? end + 1 : end;
    if (end - start > BATCH_LINE) { batch_error(shell, "A line of the batch file is too long."); shell->errorlevel = 1; continue; }
    for (int i = start; i < end; ++i) frame->line[i - start] = frame->text[i];
    frame->line[end - start] = 0;
    batch_line(shell, frame, bv(frame->line, end - start));
  }
  frame->done = 1;
}

/* --- what cmd.exe calls ------------------------------------------------------------------------------------------------------------------ */

/* Runs a command line (cmd /c ...): one line, not echoed. Returns the exit code cmd ends with. */
static unsigned batch_run_command(BatchShell *shell, BView line) {
  const BatchHost *host = shell->host;
  BatchFrame *frame = (BatchFrame *)host->alloc(host->context, sizeof(BatchFrame));
  if (!frame) { batch_error(shell, "Not enough memory."); return 1; }
  frame->text = NULL;
  frame->owned = NULL;
  frame->length = frame->position = 0;
  frame->script[0] = 0;
  frame->done = 0;
  batch_set_arguments(frame, bv(frame->script, 0), bv(frame->script, 0));
  if (line.n > BATCH_LINE) batch_error(shell, "The command line is too long.");
  else batch_line(shell, frame, line);
  host->release(host->context, frame);
  return shell->exit_all ? shell->exit_code : shell->errorlevel;
}

/* The interactive shell (cmd with neither /c nor /k, or after /k): a prompt, a line read, run, until EXIT or the end of the input. */
static unsigned batch_interactive(BatchShell *shell) {
  WCHAR here[BATCH_PATH];
  WCHAR *line = (WCHAR *)shell->host->alloc(shell->host->context, (BATCH_LINE + 1) * sizeof(WCHAR));
  if (!line) return 1;
  while (!shell->exit_all) {
    const int n = batch_cwd(shell, here, BATCH_PATH);
    batch_write(shell, shell->out, bv(here, n));
    batch_write_ascii(shell, shell->out, ">");
    const int got = shell->host->read_line(shell->host->context, line, BATCH_LINE + 1);
    if (got < 0) break;
    batch_run_command(shell, bv(line, got));
    if (!shell->exit_all) batch_write_ascii(shell, shell->out, "\r\n");
  }
  shell->host->release(shell->host->context, line);
  return shell->exit_all ? shell->exit_code : shell->errorlevel;
}
