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

// The root certificates the system trusts for https: the Mozilla bundle in certs/ca-bundle.pem is built into the image, and a bundle at
// /certs/ca-bundle.pem on the system's volume, when there is one, is used instead (so it can be updated without a new image).
#pragma once
#include "Crypto/X509.hpp"
#include "FileSystem/IFileSystem.hpp"

extern "C" const char tcos_ca_bundle_start[];
extern "C" const char tcos_ca_bundle_end[];

namespace network {

// Fills `store` with the certificates of the volume's bundle, else the built-in one. Returns how many were added.
uint32_t load_ca_bundle(crypto::TrustStore& store, filesystem::IFileSystem* fs, allocators::IAllocator& allocator);

}  // namespace network
