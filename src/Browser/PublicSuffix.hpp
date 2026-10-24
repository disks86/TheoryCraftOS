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
#pragma once
// Public suffixes: the domains that belong to a registry or a hosting service, under which unrelated parties register names (com, co.uk, github.io). A cookie
// may not name one in its Domain attribute (it would reach every site below it). The list is the part of the public suffix list that sites use most; a name with
// no dot is always a suffix (the list's implicit rule for top-level domains).
#include "Utility/StringView.hpp"

namespace browser {

// Is the domain (lower case, no leading dot) a public suffix?
bool is_public_suffix(StringView domain);

}  // namespace browser
