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

/* A DLL with thread-local data of its own (tests/windows/tlsdlltest.c uses it): every thread has its own copy, starting from the initial values. Built with clang
 * (native __declspec(thread)). */
#include <windows.h>

__declspec(thread) int g_value = 42;
__declspec(thread) char g_buffer[200];      /* (zero fill) */
__declspec(thread) int g_second = 7;

__declspec(dllexport) int tlsdll_get(void) { return g_value; }
__declspec(dllexport) void tlsdll_set(int v) { g_value = v; g_second = v + 1; g_buffer[199] = (char)v; }
__declspec(dllexport) int tlsdll_second(void) { return g_second; }
__declspec(dllexport) int tlsdll_buffer_tail(void) { return g_buffer[199]; }
__declspec(dllexport) int tlsdll_buffer_head(void) { return g_buffer[0]; }
