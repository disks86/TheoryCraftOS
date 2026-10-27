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

/* The debug-channel functions that Wine's headers expect from ntdll.dll (Wine's tests call debugstr_a() and friends through them). Wine's ntdll provides them;
 * this OS's does not, and a test only needs them to format strings, so the channels here are all off. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

const char * __cdecl __wine_dbg_strdup( const char *str ) { return strdup( str ); }
int __cdecl __wine_dbg_output( const char *str ) { return fputs( str, stderr ); }
unsigned char __cdecl __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel ) { return 0; }
int __cdecl __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel, const char *function ) { return -1; }
