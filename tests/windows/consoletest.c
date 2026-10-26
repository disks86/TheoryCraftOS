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

/* The console on TheoryCraftOS: screen buffers, cursor, attributes, modes, ANSI sequences, key events and control handlers (mingw gcc; see tests/windows/build.sh).
 * Prints "consoletest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

static int g_ctrl_calls;
static BOOL WINAPI on_ctrl(DWORD type) { g_ctrl_calls = (int)type + 1; return TRUE; }

static COORD at(int x, int y) { COORD c = {(SHORT)x, (SHORT)y}; return c; }
static void text_at(HANDLE out, int x, int y, int n, wchar_t *buffer) {
  DWORD got = 0;
  ReadConsoleOutputCharacterW(out, buffer, (DWORD)n, at(x, y), &got);
  buffer[got] = 0;
}

int main(void) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO info;
  DWORD n, mode;
  wchar_t text[200];

  /* the buffer, the cursor and written text */
  check(GetConsoleScreenBufferInfo(out, &info) && info.dwSize.X == 80 && info.dwSize.Y == 25, "buffer size");
  FillConsoleOutputCharacterW(out, L' ', 80 * 25, at(0, 0), &n);
  SetConsoleCursorPosition(out, at(0, 0));
  WriteConsoleA(out, "abc", 3, &n, NULL);
  GetConsoleScreenBufferInfo(out, &info);
  check(n == 3 && info.dwCursorPosition.X == 3 && info.dwCursorPosition.Y == 0, "cursor after text");
  text_at(out, 0, 0, 3, text);
  check(wcscmp(text, L"abc") == 0, "cells hold the text");
  WriteConsoleW(out, L"\r\nde", 4, &n, NULL);
  GetConsoleScreenBufferInfo(out, &info);
  check(info.dwCursorPosition.X == 2 && info.dwCursorPosition.Y == 1, "newline");
  text_at(out, 0, 1, 2, text);
  check(wcscmp(text, L"de") == 0, "second line");
  check(SetConsoleCursorPosition(out, at(10, 5)) && GetConsoleScreenBufferInfo(out, &info) && info.dwCursorPosition.X == 10 && info.dwCursorPosition.Y == 5, "set cursor");
  check(!SetConsoleCursorPosition(out, at(200, 5)), "cursor outside the buffer");

  /* attributes */
  SetConsoleTextAttribute(out, FOREGROUND_RED | BACKGROUND_BLUE);
  WriteConsoleA(out, "Z", 1, &n, NULL);
  WORD attr = 0;
  ReadConsoleOutputAttribute(out, &attr, 1, at(10, 5), &n);
  check(attr == (FOREGROUND_RED | BACKGROUND_BLUE), "attribute of written text");
  SetConsoleTextAttribute(out, 7);
  FillConsoleOutputAttribute(out, 0x2A, 4, at(0, 10), &n);
  FillConsoleOutputCharacterA(out, '#', 4, at(0, 10), &n);
  WORD attrs[4];
  ReadConsoleOutputAttribute(out, attrs, 4, at(0, 10), &n);
  check(n == 4 && attrs[0] == 0x2A && attrs[3] == 0x2A, "fill attribute");
  text_at(out, 0, 10, 4, text);
  check(wcscmp(text, L"####") == 0, "fill character");
  WriteConsoleOutputCharacterW(out, L"xyz", 3, at(78, 20), &n);
  check(n == 3, "output characters run on to the next line");
  text_at(out, 0, 21, 1, text);
  check(text[0] == L'z', "output characters wrap");

  /* wrapping and scrolling */
  SetConsoleCursorPosition(out, at(0, 0));
  FillConsoleOutputCharacterW(out, L' ', 80 * 25, at(0, 0), &n);
  wchar_t line[100];
  for (int i = 0; i < 85; ++i) line[i] = L'a' + (wchar_t)(i % 26);
  WriteConsoleW(out, line, 85, &n, NULL);
  GetConsoleScreenBufferInfo(out, &info);
  check(info.dwCursorPosition.Y == 1 && info.dwCursorPosition.X == 5, "line wraps at the edge");
  SetConsoleCursorPosition(out, at(0, 24));
  WriteConsoleW(out, L"last\n", 5, &n, NULL);
  GetConsoleScreenBufferInfo(out, &info);
  check(info.dwCursorPosition.Y == 24, "cursor stays on the last line when the buffer scrolls");
  text_at(out, 0, 23, 4, text);
  check(wcscmp(text, L"last") == 0, "scrolled up");
  text_at(out, 0, 0, 5, text);
  check(text[0] == L'c', "the wrapped part is now the first line");

  /* modes and ANSI sequences */
  check(GetConsoleMode(out, &mode) && (mode & ENABLE_PROCESSED_OUTPUT), "output mode");
  check(SetConsoleMode(out, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | 4), "enable virtual terminal processing");
  FillConsoleOutputCharacterW(out, L' ', 80 * 25, at(0, 0), &n);
  SetConsoleCursorPosition(out, at(0, 0));
  WriteConsoleA(out, "\x1b[3;5H\x1b[31;42mX\x1b[0m", (DWORD)strlen("\x1b[3;5H\x1b[31;42mX\x1b[0m"), &n, NULL);
  GetConsoleScreenBufferInfo(out, &info);
  check(info.dwCursorPosition.X == 5 && info.dwCursorPosition.Y == 2 && info.wAttributes == 7, "cursor position and reset sequences");
  ReadConsoleOutputAttribute(out, &attr, 1, at(4, 2), &n);
  check(attr == (FOREGROUND_RED | BACKGROUND_GREEN), "colour sequence");
  text_at(out, 4, 2, 1, text);
  check(text[0] == L'X', "text after sequences");
  WriteConsoleA(out, "\x1b[2J\x1b[Hq", (DWORD)strlen("\x1b[2J\x1b[Hq"), &n, NULL);
  text_at(out, 4, 2, 1, text);
  GetConsoleScreenBufferInfo(out, &info);
  check(text[0] == L' ' && info.dwCursorPosition.X == 1 && info.dwCursorPosition.Y == 0, "erase display and home");
  WriteConsoleA(out, "\x1b[?25l", (DWORD)strlen("\x1b[?25l"), &n, NULL);
  CONSOLE_CURSOR_INFO ci;
  check(GetConsoleCursorInfo(out, &ci) && !ci.bVisible, "hide cursor by sequence");
  ci.bVisible = TRUE; ci.dwSize = 50;
  check(SetConsoleCursorInfo(out, &ci) && GetConsoleCursorInfo(out, &ci) && ci.bVisible && ci.dwSize == 50, "cursor info");
  WriteConsoleA(out, "\x1b]0;a title\x07", (DWORD)strlen("\x1b]0;a title\x07"), &n, NULL);
  char title[64];
  check(GetConsoleTitleA(title, sizeof(title)) == 7 && strcmp(title, "a title") == 0, "title by sequence");
  check(SetConsoleTitleA("other") && GetConsoleTitleA(title, sizeof(title)) == 5 && strcmp(title, "other") == 0, "title");
  check(!SetConsoleMode(out, 0xFFFF), "invalid mode");

  /* blocks of cells */
  CHAR_INFO block[6], back[6];
  for (int i = 0; i < 6; ++i) { block[i].Char.UnicodeChar = L'A' + (wchar_t)i; block[i].Attributes = (WORD)(i + 1); }
  SMALL_RECT region = {20, 3, 22, 4};
  check(WriteConsoleOutputW(out, block, at(3, 2), at(0, 0), &region), "write a block");
  memset(back, 0, sizeof(back));
  region.Left = 20; region.Top = 3; region.Right = 22; region.Bottom = 4;
  check(ReadConsoleOutputW(out, back, at(3, 2), at(0, 0), &region) && back[4].Char.UnicodeChar == L'E' && back[4].Attributes == 5, "read a block");
  SMALL_RECT source = {20, 3, 22, 4};
  CHAR_INFO fill; fill.Char.UnicodeChar = L'.'; fill.Attributes = 7;
  check(ScrollConsoleScreenBufferW(out, &source, NULL, at(30, 10), &fill), "scroll a block");
  text_at(out, 30, 10, 3, text);
  check(wcscmp(text, L"ABC") == 0, "block moved");
  text_at(out, 20, 3, 3, text);
  check(wcscmp(text, L"...") == 0, "source filled");
  SMALL_RECT window = {0, 0, 39, 9};
  check(SetConsoleWindowInfo(out, TRUE, &window) && GetConsoleScreenBufferInfo(out, &info) && info.srWindow.Right == 39 && info.srWindow.Bottom == 9, "window");
  check(SetConsoleScreenBufferSize(out, at(100, 40)) && GetConsoleScreenBufferInfo(out, &info) && info.dwSize.X == 100 && info.dwSize.Y == 40, "buffer size change");
  text_at(out, 30, 10, 3, text);
  check(wcscmp(text, L"ABC") == 0, "text kept when the buffer grows");

  /* another screen buffer */
  HANDLE other = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
  check(other != INVALID_HANDLE_VALUE, "create a buffer");
  if (other != INVALID_HANDLE_VALUE) {
    WriteConsoleW(other, L"hidden", 6, &n, NULL);
    text_at(other, 0, 0, 6, text);
    check(wcscmp(text, L"hidden") == 0, "text in the other buffer");
    text_at(out, 0, 0, 6, text);
    check(wcsncmp(text, L"hidden", 6) != 0, "not in the first buffer");
    check(SetConsoleActiveScreenBuffer(other) && SetConsoleActiveScreenBuffer(out), "switch buffers");
    CloseHandle(other);
  }

  /* control events */
  check(SetConsoleCtrlHandler(on_ctrl, TRUE), "add a handler");
  check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) && g_ctrl_calls == 1, "Ctrl+C goes to the handler");
  check(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0) && g_ctrl_calls == 2, "Ctrl+Break goes to the handler");
  check(SetConsoleCtrlHandler(on_ctrl, FALSE) && !SetConsoleCtrlHandler(on_ctrl, FALSE), "remove a handler");

  /* input events */
  check(GetConsoleMode(in, &mode) && (mode & ENABLE_LINE_INPUT), "input mode");
  FlushConsoleInputBuffer(in);
  INPUT_RECORD rec[8];
  memset(rec, 0, sizeof(rec));
  rec[0].EventType = KEY_EVENT; rec[0].Event.KeyEvent.bKeyDown = TRUE; rec[0].Event.KeyEvent.uChar.UnicodeChar = L'q'; rec[0].Event.KeyEvent.wRepeatCount = 1;
  rec[1] = rec[0]; rec[1].Event.KeyEvent.bKeyDown = FALSE;
  check(WriteConsoleInputW(in, rec, 2, &n) && n == 2, "inject events");
  check(GetNumberOfConsoleInputEvents(in, &n) && n >= 2, "count injected events (and typed text)");
  INPUT_RECORD got[8];
  check(PeekConsoleInputW(in, got, 8, &n) && n >= 2 && got[0].Event.KeyEvent.uChar.UnicodeChar == L'q', "peek");
  check(ReadConsoleInputW(in, got, 1, &n) && n == 1 && got[0].Event.KeyEvent.bKeyDown, "read one");
  check(ReadConsoleInputW(in, got, 1, &n) && n == 1 && !got[0].Event.KeyEvent.bKeyDown, "read the rest");

  /* the typed text of the harness: a line, then the end */
  const DWORD ready = WaitForSingleObject(in, 3000);
  check(ready == WAIT_OBJECT_0, "console input is signalled when there is text or the end");
  if (GetNumberOfConsoleInputEvents(in, &n) && n >= 2) {
    check(ReadConsoleInputW(in, got, 2, &n) && n == 2 && got[0].Event.KeyEvent.bKeyDown && !got[1].Event.KeyEvent.bKeyDown, "typed text becomes key down and key up");
    check(got[0].Event.KeyEvent.uChar.UnicodeChar == L'k' && got[0].Event.KeyEvent.wVirtualKeyCode == 'K', "first key");
    char rest[64];
    DWORD count = 0;
    check(ReadConsole(in, rest, sizeof(rest), &count, NULL) && count > 2 && rest[count - 2] == '\r' && rest[count - 1] == '\n', "the rest of the line is read as a line");
  }
  check(ReadConsoleInputW(in, got, 1, &n) && n == 0, "end of input gives no events");

  printf("consoletest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
