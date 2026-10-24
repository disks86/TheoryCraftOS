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


// A development aid: building with -DH265_TRACE makes every place the decoder refuses its input say where (file and line) on stderr. Without it, H265_FALSE is false.
#pragma once
#ifdef H265_TRACE
#include <stdio.h>
#define H265_FALSE (fprintf(stderr, "h265: refused at %s:%d\n", __FILE__, __LINE__), false)
#else
#define H265_FALSE false
#endif
