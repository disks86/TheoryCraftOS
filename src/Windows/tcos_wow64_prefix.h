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

/* Included before every source of the 32-bit libraries (scripts/build-wow64-dlls.sh, docs/Wow64.md): what the 64-bit build gets on its own and the
 * i386 one does not. Not for the assembly files. */
#if !defined(__ASSEMBLER__) && !defined(offsetof)
#define offsetof(TYPE, MEMBER) __builtin_offsetof (TYPE, MEMBER)      /* (spelled as the compiler's stddef.h spells it, which may come later) */
#endif
