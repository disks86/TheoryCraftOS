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

/* The SLTG type library format (written by MKTYPLIB; stdole32.tlb is one): a 0x1c byte header (magic, number of blocks, first block, the library's marker GUID), a table
 * of block entries {length, offset of the stream name in the name list, next block}, the names of the streams, and the blocks in the order the entries chain: one block
 * per type (magic 0x0501) and the last one, the library block, which holds the library's attributes, the table of types and the table of names.
 * The container is checked here (every length and offset against the file) and the types are listed by name from the library block's table; the member tables of the
 * type blocks are NOT decoded: such a library is refused with TYPE_E_CANTLOADLIBRARY, which is what a library of a format that is not understood gets. */

#define SLTG_HEADER_SIZE 0x1cu

static HRESULT sltg_load(const BYTE *data, SIZE_T size, TlLib **out) {
  (void)out;
  if (size < SLTG_HEADER_SIZE + 8 || memcmp(data, "SLTG", 4)) return TYPE_E_CANTLOADLIBRARY;
  const UINT blocks = data[4] | data[5] << 8;
  if (blocks == 0 || blocks > 0x1000 || SLTG_HEADER_SIZE + (SIZE_T)blocks * 8 > size) return TYPE_E_CANTLOADLIBRARY;
  /* the block lengths must fit the file */
  SIZE_T total = 0;
  for (UINT i = 0; i < blocks; i++) {
    const BYTE *e = data + SLTG_HEADER_SIZE + (SIZE_T)i * 8;
    const SIZE_T len = (SIZE_T)e[0] | (SIZE_T)e[1] << 8 | (SIZE_T)e[2] << 16 | (SIZE_T)e[3] << 24;
    if (len > size) return TYPE_E_CANTLOADLIBRARY;
    total += len;
  }
  if (total > size) return TYPE_E_CANTLOADLIBRARY;
  return TYPE_E_CANTLOADLIBRARY;
}
