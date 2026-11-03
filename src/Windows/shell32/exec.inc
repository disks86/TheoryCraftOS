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

/* ShellExecute and the file associations behind it: what a file name or URL means is found in the registry (HKEY_CLASSES_ROOT: an extension names a class, the class has
 * shell\<verb>\command), programs are started with CreateProcess. Included by shell32.c. */

#define SE_RESULT_OK 33      /* hInstApp on success: anything above 32 */

/* ---- associations ------------------------------------------------------------------------------------------------------------------------------------------ */

static BOOL classes_string(const WCHAR *key, const WCHAR *value, WCHAR *out, DWORD capacity) { return reg_get_string(HKEY_CLASSES_ROOT, key, value, out, capacity) && out[0]; }

/* The class (ProgID) of a file name: its extension's default value, or the extension itself when it has a shell key of its own. */
static BOOL file_class(const WCHAR *file, WCHAR *out, size_t capacity) {
  const WCHAR *ext = path_extension(file);
  if (!*ext || !ext[1]) return FALSE;
  WCHAR key[MAX_PATH_EX];
  w_copy(key, ARRAY_LEN(key), ext);
  /* a user's choice wins */
  WCHAR choice[MAX_PATH_EX];
  w_copy(choice, ARRAY_LEN(choice), L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\");
  w_cat(choice, ARRAY_LEN(choice), ext);
  w_cat(choice, ARRAY_LEN(choice), L"\\UserChoice");
  if (reg_get_string(HKEY_CURRENT_USER, choice, L"ProgId", out, (DWORD)capacity) && out[0]) return TRUE;
  if (classes_string(key, NULL, out, (DWORD)capacity)) return TRUE;
  w_cat(key, ARRAY_LEN(key), L"\\shell");
  if (reg_key_exists(HKEY_CLASSES_ROOT, key)) { w_copy(out, capacity, ext); return TRUE; }
  return FALSE;
}
/* The command of a verb of a class (or of "*", "Directory", "Folder", "Drive" ...); `verb` NULL or empty takes the class's default verb. */
static BOOL class_command(const WCHAR *class_name, const WCHAR *verb, WCHAR *command, size_t capacity, WCHAR *found_verb) {
  WCHAR key[MAX_PATH_EX], chosen[80];
  chosen[0] = 0;
  if (verb && *verb) w_copy(chosen, ARRAY_LEN(chosen), verb);
  else {
    w_copy(key, ARRAY_LEN(key), class_name);
    w_cat(key, ARRAY_LEN(key), L"\\shell");
    if (!classes_string(key, NULL, chosen, ARRAY_LEN(chosen))) {
      /* the default is "open" when the class has it, otherwise the first verb */
      WCHAR open[MAX_PATH_EX];
      w_copy(open, ARRAY_LEN(open), key);
      w_cat(open, ARRAY_LEN(open), L"\\open");
      if (reg_key_exists(HKEY_CLASSES_ROOT, open)) w_copy(chosen, ARRAY_LEN(chosen), L"open");
      else {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &k) == ERROR_SUCCESS) {
          DWORD n = ARRAY_LEN(chosen);
          if (RegEnumKeyExW(k, 0, chosen, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) chosen[0] = 0;
          RegCloseKey(k);
        }
      }
    }
  }
  if (!chosen[0]) return FALSE;
  w_copy(key, ARRAY_LEN(key), class_name);
  w_cat(key, ARRAY_LEN(key), L"\\shell\\");
  w_cat(key, ARRAY_LEN(key), chosen);
  w_cat(key, ARRAY_LEN(key), L"\\command");
  if (!classes_string(key, NULL, command, (DWORD)capacity)) return FALSE;
  if (found_verb) w_copy(found_verb, 80, chosen);
  return TRUE;
}

/* ---- command templates ------------------------------------------------------------------------------------------------------------------------------------- */

/* The arguments of a parameter string the way the shell splits them for %2 .. %9 (not the way CreateProcess or CommandLineToArgv do): spaces separate; a quote that
 * opens an argument starts a quoted part; a run of k quotes inside a quoted part (the opening one counted) is k/2 quote characters, and an odd run also closes it and ends the
 * argument; outside a quoted part a run of k quotes is k/2 quote characters and, when k is odd, the end of the argument. */
static size_t split_parameters(const WCHAR *params, WCHAR args[9][MAX_PATH_EX], size_t ends[9]) {
  size_t n = 0, len = 0;
  BOOL quoted = FALSE, open_arg = FALSE;
  const WCHAR *s = params ? params : L"";
#define EMIT(c) do { if (n < 9 && len + 1 < MAX_PATH_EX) args[n][len++] = (c); } while (0)
#define END_ARG() do { if (open_arg) { if (n < 9) { args[n][len] = 0; ends[n] = (size_t)(s - (params ? params : L"")); } if (n < 9) ++n; } len = 0; open_arg = FALSE; quoted = FALSE; } while (0)
  while (*s && n < 9) {
    if (*s == '"') {
      size_t run = 0;
      while (s[run] == '"') ++run;
      s += run;
      size_t k = run;
      if (!quoted && !open_arg) { quoted = TRUE; open_arg = TRUE; --k; }      /* (the opening quote is part of the run) */
      else if (!quoted) open_arg = TRUE;
      if (quoted) {
        for (size_t i = 0; i < k / 2; ++i) EMIT('"');
        if (k % 2) END_ARG();
      } else {
        for (size_t i = 0; i < k / 2; ++i) EMIT('"');
        if (k % 2) END_ARG();
      }
      continue;
    }
    if (*s == ' ' && !quoted) { END_ARG(); ++s; continue; }
    open_arg = TRUE;
    EMIT(*s);
    ++s;
  }
  if (n < 9) END_ARG();
#undef EMIT
#undef END_ARG
  return n;
}
/* Fills in %1 %L %* %2-%9 %% of a command template. `used_file` tells whether the file name was placed. */
static BOOL substitute_template(const WCHAR *templ, const WCHAR *file, const WCHAR *params, WCHAR *out, size_t capacity, BOOL *used_file) {
  WCHAR args[9][MAX_PATH_EX];
  size_t ends[9] = {0};
  const size_t arg_count = split_parameters(params, args, ends);
  size_t n = 0;
  if (used_file) *used_file = FALSE;
#define PUT(c) do { if (n + 1 >= capacity) return FALSE; out[n++] = (c); } while (0)
  for (const WCHAR *s = templ; *s; ++s) {
    if (*s != '%') { PUT(*s); continue; }
    const WCHAR c = s[1];
    const WCHAR *text = NULL;
    if (c == '%') { PUT('%'); ++s; continue; }
    if (c == '1' || c == 'L' || c == 'l' || c == 'W' || c == 'w' || c == '0') { text = file; if (used_file) *used_file = TRUE; }
    else if (c == '*') text = params ? params : L"";
    else if (c == '~' && s[2] >= '2' && s[2] <= '9') {
      /* the rest of the parameters, from where argument N starts (what follows argument N-1) */
      const size_t index = (size_t)(s[2] - '2');
      const WCHAR *all = params ? params : L"";
      size_t start = 0;
      if (index > 0) start = index - 1 < arg_count ? ends[index - 1] : wlen(all);
      text = all + start;
      s += 1;
    }
    else if (c >= '2' && c <= '9') { const size_t idx = (size_t)(c - '2'); text = idx < arg_count ? args[idx] : L""; }
    else if (c == 'V' || c == 'v') { text = file; if (used_file) *used_file = TRUE; }
    else if (c == 'D' || c == 'd') { text = L""; }
    else if (c == 'I' || c == 'i' || c == 'S' || c == 's' || c == 'H' || c == 'h' || c == 'C' || c == 'c' || c == 'U' || c == 'u') { text = L""; }
    else { PUT('%'); continue; }
    ++s;
    for (const WCHAR *t = text; *t; ++t) PUT(*t);
  }
  out[n] = 0;
#undef PUT
  return TRUE;
}

/* ---- running ------------------------------------------------------------------------------------------------------------------------------------------------- */

static int se_error_from_win32(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND: return SE_ERR_FNF;
    case ERROR_PATH_NOT_FOUND: return SE_ERR_PNF;
    case ERROR_ACCESS_DENIED: return SE_ERR_ACCESSDENIED;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return SE_ERR_OOM;
    case ERROR_BAD_EXE_FORMAT: case ERROR_BAD_FORMAT: return 11;      /* ERROR_BAD_FORMAT */
    case ERROR_SHARING_VIOLATION: return SE_ERR_SHARE;
    case ERROR_DLL_NOT_FOUND: case ERROR_MOD_NOT_FOUND: return SE_ERR_DLLNOTFOUND;
    default: return SE_ERR_ACCESSDENIED;
  }
}
/* Starts a command line; the process handle is given back if `process` is not NULL. */
static BOOL start_process(const WCHAR *application, WCHAR *command_line, const WCHAR *directory, int show, HANDLE *process, DWORD *error) {
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = (WORD)show;
  ZeroMemory(&pi, sizeof(pi));
  if (!CreateProcessW(application, command_line, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP * 0, NULL, directory && *directory ? directory : NULL, &si, &pi)) {
    *error = GetLastError();
    return FALSE;
  }
  CloseHandle(pi.hThread);
  if (process) *process = pi.hProcess; else CloseHandle(pi.hProcess);
  return TRUE;
}
/* The program a command line starts: its first token (quoted or not), expanded and found. */
static BOOL command_program(const WCHAR *command, WCHAR *program, size_t capacity) {
  WCHAR work[MAX_PATH_EX];
  const WCHAR *s = command;
  while (*s == ' ') ++s;
  if (*s == '"') {
    const WCHAR *end = w_find(s + 1, '"');
    if (!end) return FALSE;
    if (!w_copy_n(work, ARRAY_LEN(work), s + 1, (size_t)(end - s - 1))) return FALSE;
  } else {
    /* the longest start that is an existing file, otherwise the first token */
    size_t n = wlen(s), best = 0;
    for (size_t i = 1; i <= n; ++i) {
      if (s[i] && s[i] != ' ') continue;
      WCHAR trial[MAX_PATH_EX];
      w_copy_n(trial, ARRAY_LEN(trial), s, i);
      if (path_exists(trial) && !path_is_dir(trial)) best = i;
    }
    if (!best) { const WCHAR *sp = w_find(s, ' '); best = sp ? (size_t)(sp - s) : n; }
    if (!w_copy_n(work, ARRAY_LEN(work), s, best)) return FALSE;
  }
  WCHAR expanded[MAX_PATH_EX];
  const DWORD n = ExpandEnvironmentStringsW(work, expanded, ARRAY_LEN(expanded));
  if (!n || n > ARRAY_LEN(expanded)) return FALSE;
  return w_copy(program, capacity, expanded);
}

typedef struct {
  const WCHAR *verb, *file, *parameters, *directory;
  int show;
  DWORD mask;
  HANDLE process;
  int error;      /* an SE_ERR_ value, or 0 */
  DWORD win32_error;
} ExecRequest;

static BOOL is_url(const WCHAR *s) {
  size_t i = 0;
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return FALSE;
  for (; s[i] && s[i] != ':'; ++i) {
    const WCHAR c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return FALSE;
  }
  return s[i] == ':' && i >= 2;
}

/* Runs a class's command for a file or URL. */
static BOOL run_class_command(ExecRequest *r, const WCHAR *class_name, const WCHAR *file) {
  WCHAR command[MAX_PATH_EX * 2], line[MAX_PATH_EX * 2], program[MAX_PATH_EX];
  BOOL used = FALSE;
  if (!class_command(class_name, r->verb, command, ARRAY_LEN(command), NULL)) { r->error = SE_ERR_NOASSOC; r->win32_error = ERROR_NO_ASSOCIATION; return FALSE; }
  WCHAR expanded[MAX_PATH_EX * 2];
  if (!ExpandEnvironmentStringsW(command, expanded, ARRAY_LEN(expanded))) w_copy(expanded, ARRAY_LEN(expanded), command);
  /* the file is quoted when it has spaces and the template does not already quote it */
  WCHAR quoted[MAX_PATH_EX];
  w_copy(quoted, ARRAY_LEN(quoted), file);
  if (!substitute_template(expanded, quoted, r->parameters, line, ARRAY_LEN(line), &used)) { r->error = SE_ERR_ACCESSDENIED; return FALSE; }
  if (!used && !w_find(command, '%')) {
    /* a command without any place for the file gets it appended */
    if (!w_cat(line, ARRAY_LEN(line), L" ") || !w_cat(line, ARRAY_LEN(line), needs_quotes(file) ? L"\"" : L"") || !w_cat(line, ARRAY_LEN(line), file) || !w_cat(line, ARRAY_LEN(line), needs_quotes(file) ? L"\"" : L"")) return FALSE;
  }
  DWORD error = 0;
  program[0] = 0;
  command_program(line, program, ARRAY_LEN(program));
  if (!start_process(NULL, line, r->directory, r->show, (r->mask & SEE_MASK_NOCLOSEPROCESS) ? &r->process : NULL, &error)) {
    r->error = se_error_from_win32(error);
    r->win32_error = error;
    return FALSE;
  }
  return TRUE;
}

static BOOL execute_request(ExecRequest *r) {
  WCHAR file[MAX_PATH_EX], full[MAX_PATH_EX], dir[MAX_PATH_EX];
  r->error = SE_ERR_FNF;
  r->win32_error = ERROR_FILE_NOT_FOUND;
  if (!r->file || !*r->file) { r->win32_error = ERROR_INVALID_PARAMETER; r->error = SE_ERR_FNF; return FALSE; }
  if (!w_copy(file, ARRAY_LEN(file), r->file)) { r->error = SE_ERR_ACCESSDENIED; r->win32_error = ERROR_FILENAME_EXCED_RANGE; return FALSE; }
  if (r->mask & SEE_MASK_DOENVSUBST) {
    WCHAR e[MAX_PATH_EX];
    if (ExpandEnvironmentStringsW(file, e, ARRAY_LEN(e))) w_copy(file, ARRAY_LEN(file), e);
  }
  path_unquote(file);
  if (r->directory && *r->directory) w_copy(dir, ARRAY_LEN(dir), r->directory); else GetCurrentDirectoryW(ARRAY_LEN(dir), dir);
  /* a URL */
  if (is_url(file)) {
    WCHAR scheme[40];
    const WCHAR *colon = w_find(file, ':');
    w_copy_n(scheme, ARRAY_LEN(scheme), file, (size_t)(colon - file));
    WCHAR protocol_key[MAX_PATH_EX];
    w_copy(protocol_key, ARRAY_LEN(protocol_key), scheme);
    if (!(scheme[1] == 0) && reg_key_exists(HKEY_CLASSES_ROOT, protocol_key)) return run_class_command(r, scheme, file);
    if (wlen(scheme) > 1) { r->error = SE_ERR_NOASSOC; r->win32_error = ERROR_NO_ASSOCIATION; return FALSE; }
  }
  /* a file: relative to the given directory, or found on the search path */
  if (is_relative(file) || (!has_drive(file) && !is_unc(file))) {
    WCHAR joined[MAX_PATH_EX];
    if (path_join(joined, ARRAY_LEN(joined), dir, file) && (path_exists(joined))) w_copy(full, ARRAY_LEN(full), joined);
    else {
      w_copy(full, ARRAY_LEN(full), file);
      const WCHAR *none[] = {dir, NULL};
      if (!w_find(file, '\\') && !w_find(file, '/') && !find_on_path(full, none, TRUE)) { r->error = SE_ERR_FNF; r->win32_error = ERROR_FILE_NOT_FOUND; w_copy(full, ARRAY_LEN(full), file); }
      else if (w_find(file, '\\') || w_find(file, '/')) path_full(file, full, ARRAY_LEN(full));
    }
  } else if (!path_full(file, full, ARRAY_LEN(full))) w_copy(full, ARRAY_LEN(full), file);
  DWORD attributes;
  if (!path_attributes(full, &attributes)) {
    WCHAR parent[MAX_PATH_EX];
    w_copy_n(parent, ARRAY_LEN(parent), full, (size_t)(path_file_name(full) - full));
    path_strip_trailing_sep(parent);
    r->error = parent[0] && !path_is_dir(parent) && !(has_drive(parent) && parent[2] == 0) ? SE_ERR_PNF : SE_ERR_FNF;
    r->win32_error = r->error == SE_ERR_PNF ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND;
    return FALSE;
  }
  const BOOL is_dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const WCHAR *verb = r->verb && *r->verb ? r->verb : NULL;
  if (is_dir) {
    WCHAR command[MAX_PATH_EX * 2];
    static const WCHAR *const classes[] = {L"Directory", L"Folder"};
    for (size_t i = 0; i < ARRAY_LEN(classes); ++i) if (class_command(classes[i], verb, command, ARRAY_LEN(command), NULL)) return run_class_command(r, classes[i], full);
    /* no handler: the system's explorer, if the machine has one */
    WCHAR explorer[MAX_PATH_EX];
    if (GetWindowsDirectoryW(explorer, ARRAY_LEN(explorer)) && path_add_sep(explorer, ARRAY_LEN(explorer)) && w_cat(explorer, ARRAY_LEN(explorer), L"explorer.exe") && path_exists(explorer)) {
      WCHAR line[MAX_PATH_EX * 2];
      w_copy(line, ARRAY_LEN(line), L"\"");
      w_cat(line, ARRAY_LEN(line), explorer);
      w_cat(line, ARRAY_LEN(line), L"\" \"");
      w_cat(line, ARRAY_LEN(line), full);
      w_cat(line, ARRAY_LEN(line), L"\"");
      DWORD error;
      if (start_process(explorer, line, dir, r->show, (r->mask & SEE_MASK_NOCLOSEPROCESS) ? &r->process : NULL, &error)) return TRUE;
      r->error = se_error_from_win32(error);
      r->win32_error = error;
      return FALSE;
    }
    r->error = SE_ERR_NOASSOC;
    r->win32_error = ERROR_NO_ASSOCIATION;
    return FALSE;
  }
  /* a program: started as it is; other verbs of its class (edit, print ...) come from the registry */
  const WCHAR *ext = path_extension(full);
  const BOOL program = is_program_extension(ext);
  if (program && (!verb || !wicmp(verb, L"open") || !wicmp(verb, L"runas") || !wicmp(verb, L"runasuser"))) {
    WCHAR line[MAX_PATH_EX * 2];
    line[0] = 0;
    w_cat(line, ARRAY_LEN(line), L"\"");
    w_cat(line, ARRAY_LEN(line), full);
    w_cat(line, ARRAY_LEN(line), L"\"");
    if (r->parameters && *r->parameters) { w_cat(line, ARRAY_LEN(line), L" "); w_cat(line, ARRAY_LEN(line), r->parameters); }
    DWORD error;
    if (!start_process(NULL, line, r->directory ? r->directory : NULL, r->show, (r->mask & SEE_MASK_NOCLOSEPROCESS) ? &r->process : NULL, &error)) {
      r->error = se_error_from_win32(error);
      r->win32_error = error;
      return FALSE;
    }
    return TRUE;
  }
  /* any file: the class of its extension, then the classes of every file */
  WCHAR class_name[MAX_PATH_EX], command[MAX_PATH_EX * 2];
  if (file_class(full, class_name, ARRAY_LEN(class_name)) && class_command(class_name, verb, command, ARRAY_LEN(command), NULL)) return run_class_command(r, class_name, full);
  if (class_command(L"*", verb, command, ARRAY_LEN(command), NULL)) return run_class_command(r, L"*", full);
  if (class_command(L"Unknown", verb, command, ARRAY_LEN(command), NULL) && verb) return run_class_command(r, L"Unknown", full);
  r->error = SE_ERR_NOASSOC;
  r->win32_error = ERROR_NO_ASSOCIATION;
  return FALSE;
}

EXPORT BOOL WINAPI ShellExecuteExW(SHELLEXECUTEINFOW *info) {
  if (!info || info->cbSize < sizeof(SHELLEXECUTEINFOW) - sizeof(HANDLE) * 0) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
  ExecRequest r;
  ZeroMemory(&r, sizeof(r));
  WCHAR from_pidl[MAX_PATH_EX];
  r.verb = info->lpVerb;
  r.file = info->lpFile;
  r.parameters = info->lpParameters;
  r.directory = info->lpDirectory;
  r.show = info->nShow;
  r.mask = info->fMask;
  if ((info->fMask & (SEE_MASK_IDLIST | SEE_MASK_INVOKEIDLIST)) && info->lpIDList) {
    if (SHGetPathFromIDListW(info->lpIDList, from_pidl)) r.file = from_pidl;
    else { info->hInstApp = (HINSTANCE)(ULONG_PTR)SE_ERR_NOASSOC; SetLastError(ERROR_NO_ASSOCIATION); return FALSE; }
  }
  info->hProcess = NULL;
  const BOOL ok = execute_request(&r);
  info->hInstApp = (HINSTANCE)(ULONG_PTR)(ok ? SE_RESULT_OK : r.error);
  if (ok) info->hProcess = r.process;
  else SetLastError(r.win32_error ? r.win32_error : ERROR_FILE_NOT_FOUND);
  return ok;
}
EXPORT BOOL WINAPI ShellExecuteExA(SHELLEXECUTEINFOA *info) {
  if (!info || info->cbSize < sizeof(SHELLEXECUTEINFOA)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
  SHELLEXECUTEINFOW w;
  ZeroMemory(&w, sizeof(w));
  w.cbSize = sizeof(w);
  w.fMask = info->fMask;
  w.hwnd = info->hwnd;
  WCHAR *verb = a_to_w(info->lpVerb), *file = a_to_w(info->lpFile), *params = a_to_w(info->lpParameters), *dir = a_to_w(info->lpDirectory), *cls = (info->fMask & SEE_MASK_CLASSNAME) ? a_to_w(info->lpClass) : NULL;
  w.lpVerb = verb; w.lpFile = file; w.lpParameters = params; w.lpDirectory = dir;
  w.nShow = info->nShow;
  w.lpIDList = info->lpIDList;
  w.lpClass = cls;
  w.hkeyClass = info->hkeyClass;
  const BOOL ok = ShellExecuteExW(&w);
  info->hInstApp = w.hInstApp;
  info->hProcess = w.hProcess;
  mem_free(verb); mem_free(file); mem_free(params); mem_free(dir); mem_free(cls);
  return ok;
}
EXPORT BOOL WINAPI ShellExecuteEx(SHELLEXECUTEINFOW *info) { return ShellExecuteExW(info); }
EXPORT HINSTANCE WINAPI ShellExecuteW(HWND owner, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show) {
  SHELLEXECUTEINFOW info;
  ZeroMemory(&info, sizeof(info));
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI;
  info.hwnd = owner; info.lpVerb = verb; info.lpFile = file; info.lpParameters = params; info.lpDirectory = dir; info.nShow = show;
  ShellExecuteExW(&info);
  return info.hInstApp;
}
EXPORT HINSTANCE WINAPI ShellExecuteA(HWND owner, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, INT show) {
  SHELLEXECUTEINFOA info;
  ZeroMemory(&info, sizeof(info));
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI;
  info.hwnd = owner; info.lpVerb = verb; info.lpFile = file; info.lpParameters = params; info.lpDirectory = dir; info.nShow = show;
  ShellExecuteExA(&info);
  return info.hInstApp;
}
EXPORT BOOL WINAPI RealShellExecuteExW(SHELLEXECUTEINFOW *info) { return ShellExecuteExW(info); }
EXPORT BOOL WINAPI RealShellExecuteExA(SHELLEXECUTEINFOA *info) { return ShellExecuteExA(info); }
EXPORT HINSTANCE WINAPI RealShellExecuteW(HWND owner, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, LPWSTR result, LPCWSTR title, LPWSTR reserved, WORD show, PHANDLE process) {
  (void)result; (void)title; (void)reserved; (void)process;
  return ShellExecuteW(owner, verb, file, params, dir, show);
}
EXPORT HINSTANCE WINAPI RealShellExecuteA(HWND owner, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, LPSTR result, LPCSTR title, LPSTR reserved, WORD show, PHANDLE process) {
  (void)result; (void)title; (void)reserved; (void)process;
  return ShellExecuteA(owner, verb, file, params, dir, show);
}
EXPORT HINSTANCE WINAPI WOWShellExecute(HWND owner, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, INT show, void *callback) {
  (void)callback;
  return ShellExecuteA(owner, verb, file, params, dir, show);
}

/* ---- FindExecutable ------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HINSTANCE WINAPI FindExecutableW(LPCWSTR file, LPCWSTR directory, LPWSTR result) {
  WCHAR full[MAX_PATH_EX], dir[MAX_PATH_EX];
  if (result) result[0] = 0;
  if (!file || !result) { SetLastError(ERROR_INVALID_PARAMETER); return (HINSTANCE)(ULONG_PTR)SE_ERR_FNF; }
  if (directory && *directory) w_copy(dir, ARRAY_LEN(dir), directory); else GetCurrentDirectoryW(ARRAY_LEN(dir), dir);
  if (is_relative(file) || (!has_drive(file) && !is_unc(file))) {
    WCHAR joined[MAX_PATH_EX];
    if (!path_join(joined, ARRAY_LEN(joined), dir, file) || !path_exists(joined)) { SetLastError(ERROR_FILE_NOT_FOUND); return (HINSTANCE)(ULONG_PTR)SE_ERR_FNF; }
    w_copy(full, ARRAY_LEN(full), joined);
  } else if (!path_exists(file)) { SetLastError(ERROR_FILE_NOT_FOUND); return (HINSTANCE)(ULONG_PTR)SE_ERR_FNF; }
  else w_copy(full, ARRAY_LEN(full), file);
  if (path_is_dir(full)) { SetLastError(ERROR_NO_ASSOCIATION); return (HINSTANCE)(ULONG_PTR)SE_ERR_NOASSOC; }
  path_full(full, full, ARRAY_LEN(full));
  if (is_program_extension(path_extension(full))) {
    if (wlen(full) >= MAX_PATH) return (HINSTANCE)(ULONG_PTR)SE_ERR_ACCESSDENIED;
    w_copy(result, MAX_PATH, full);
    return (HINSTANCE)(ULONG_PTR)SE_RESULT_OK;
  }
  WCHAR class_name[MAX_PATH_EX], command[MAX_PATH_EX * 2], program[MAX_PATH_EX];
  if (file_class(full, class_name, ARRAY_LEN(class_name)) && (class_command(class_name, L"open", command, ARRAY_LEN(command), NULL) || class_command(class_name, NULL, command, ARRAY_LEN(command), NULL))
      && command_program(command, program, ARRAY_LEN(program))) {
    WCHAR located[MAX_PATH_EX];
    w_copy(located, ARRAY_LEN(located), program);
    const WCHAR *none[] = {NULL};
    if (!path_exists(located) && !find_on_path(located, none, FALSE)) { SetLastError(ERROR_FILE_NOT_FOUND); return (HINSTANCE)(ULONG_PTR)SE_ERR_FNF; }
    if (wlen(located) >= MAX_PATH) return (HINSTANCE)(ULONG_PTR)SE_ERR_ACCESSDENIED;
    w_copy(result, MAX_PATH, located);
    return (HINSTANCE)(ULONG_PTR)SE_RESULT_OK;
  }
  SetLastError(ERROR_NO_ASSOCIATION);
  return (HINSTANCE)(ULONG_PTR)SE_ERR_NOASSOC;
}
EXPORT HINSTANCE WINAPI FindExecutableA(LPCSTR file, LPCSTR directory, LPSTR result) {
  WCHAR *f = a_to_w(file), *d = a_to_w(directory);
  WCHAR out[MAX_PATH];
  HINSTANCE r = (HINSTANCE)(ULONG_PTR)SE_ERR_FNF;
  if (result) result[0] = 0;
  if (f && result) {
    r = FindExecutableW(f, d, out);
    if ((ULONG_PTR)r > 32) w_to_a(out, result, MAX_PATH);
  }
  mem_free(f); mem_free(d);
  return r;
}

/* ---- templates, processes and the Rundll entry points ---------------------------------------------------------------------------------------------------- */

/* A command template split into the program, the whole command line and the parameters. The strings are CoTaskMem allocations. */
EXPORT HRESULT WINAPI SHEvaluateSystemCommandTemplate(PCWSTR templ, PWSTR *application, PWSTR *command_line, PWSTR *parameters) {
  if (application) *application = NULL;
  if (command_line) *command_line = NULL;
  if (parameters) *parameters = NULL;
  if (!templ || !application || !command_line) return E_INVALIDARG;
  WCHAR expanded[MAX_PATH_EX * 2], program[MAX_PATH_EX];
  if (!ExpandEnvironmentStringsW(templ, expanded, ARRAY_LEN(expanded))) return hr_last_error();
  if (!command_program(expanded, program, ARRAY_LEN(program))) return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
  WCHAR located[MAX_PATH_EX];
  w_copy(located, ARRAY_LEN(located), program);
  const WCHAR *none[] = {NULL};
  if (!path_exists(located) && !find_on_path(located, none, TRUE)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  /* what follows the program in the template */
  const WCHAR *s = expanded;
  while (*s == ' ') ++s;
  if (*s == '"') { const WCHAR *end = w_find(s + 1, '"'); s = end ? end + 1 : s + wlen(s); }
  else { const WCHAR *sp = w_find(s, ' '); s = sp ? sp : s + wlen(s); }
  while (*s == ' ') ++s;
  *application = w_task_dup(located);
  *command_line = w_task_dup(expanded);
  if (parameters) *parameters = w_task_dup(s);
  return *application && *command_line ? S_OK : E_OUTOFMEMORY;
}

EXPORT BOOL WINAPI SHCreateProcessAsUserW(SHCREATEPROCESSINFOW *info) {
  if (!info || info->cbSize < sizeof(*info)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!info->pszFile) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR line[MAX_PATH_EX * 2];
  line[0] = 0;
  w_cat(line, ARRAY_LEN(line), L"\"");
  w_cat(line, ARRAY_LEN(line), info->pszFile);
  w_cat(line, ARRAY_LEN(line), L"\"");
  if (info->pszParameters && *info->pszParameters) { w_cat(line, ARRAY_LEN(line), L" "); w_cat(line, ARRAY_LEN(line), info->pszParameters); }
  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  if (info->lpStartupInfo) si = *info->lpStartupInfo;
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  if (!CreateProcessW(NULL, line, info->lpProcessAttributes, info->lpThreadAttributes, info->bInheritHandles, info->dwCreationFlags, NULL, info->pszCurrentDirectory, &si, &pi)) return FALSE;
  if (info->lpProcessInformation) *info->lpProcessInformation = pi; else { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
  return TRUE;
}

/* "Rundll32 shell32.dll,ShellExec_RunDLL file parameters": opens the file named by the command line. */
EXPORT void WINAPI ShellExec_RunDLLW(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) {
  (void)instance;
  WCHAR *copy = w_dup(command_line);
  if (!copy) return;
  WCHAR *file = copy;
  while (*file == ' ') ++file;
  WCHAR *params = NULL;
  if (*file == '"') { WCHAR *end = (WCHAR *)w_find(file + 1, '"'); ++file; if (end) { *end = 0; params = end + 1; } }
  else { WCHAR *sp = (WCHAR *)w_find(file, ' '); if (sp) { *sp = 0; params = sp + 1; } }
  if (params) while (*params == ' ') ++params;
  ShellExecuteW(owner, NULL, file, params, NULL, show ? show : SW_SHOWNORMAL);
  mem_free(copy);
}
EXPORT void WINAPI ShellExec_RunDLL(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { ShellExec_RunDLLW(owner, instance, command_line, show); }
EXPORT void WINAPI ShellExec_RunDLLA(HWND owner, HINSTANCE instance, LPCSTR command_line, int show) {
  WCHAR *w = a_to_w(command_line);
  ShellExec_RunDLLW(owner, instance, w, show);
  mem_free(w);
}
/* The control panel and the other dialogs of the RunDLL family need windows: they do nothing here but succeed in saying so. */
#define RUNDLL_NOTHING(name) \
  EXPORT void WINAPI name##W(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; } \
  EXPORT void WINAPI name##A(HWND owner, HINSTANCE instance, LPCSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; } \
  EXPORT void WINAPI name(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; }
RUNDLL_NOTHING(Control_RunDLL)
RUNDLL_NOTHING(OpenAs_RunDLL)
RUNDLL_NOTHING(Options_RunDLL)
RUNDLL_NOTHING(SHHelpShortcuts_RunDLL)
RUNDLL_NOTHING(PrintersGetCommand_RunDLL)
RUNDLL_NOTHING(Control_FillCache_RunDLL)
#undef RUNDLL_NOTHING
EXPORT void WINAPI Activate_RunDLL(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; }
EXPORT BOOL WINAPI SHRunControlPanel(LPCWSTR command_line, HWND owner) { (void)command_line; (void)owner; return FALSE; }
EXPORT HRESULT WINAPI SHOpenWithDialog(HWND owner, const OPENASINFO *info) { (void)owner; (void)info; return HRESULT_FROM_WIN32(ERROR_CANCELLED); }
EXPORT HRESULT WINAPI AssocCreateForClasses(const ASSOCIATIONELEMENT *classes, ULONG count, REFIID riid, void **out) { (void)classes; (void)count; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHAssocEnumHandlers(PCWSTR ext, ASSOC_FILTER filter, IEnumAssocHandlers **out) { (void)ext; (void)filter; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHAssocEnumHandlersForProtocolByApplication(PCWSTR protocol, REFIID riid, void **out) { (void)protocol; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHCreateAssociationRegistration(REFIID riid, void **out) { (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHCreateDefaultExtractIcon(REFIID riid, void **out) { (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
