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

// What the browser saves of its service worker registrations (Desktop/BrowserStore.cpp keeps it in the system database), and the interface the saving goes through: the
// hub (ServiceWorker.hpp) implements it, so the store does not depend on everything the hub runs workers with.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace browser {

// A registration as the browser saves it. The worker that was active comes back active. `imports` holds the scripts the worker imported, as "url\nlength\nbytes" for each.
struct SavedRegistration {
  explicit SavedRegistration(allocators::IAllocator& a) : origin(a), scope(a), script_url(a), update_via_cache(a), type(a), source(a), imports(a), preload_header(a) {}
  String origin, scope, script_url, update_via_cache, type, source, imports;
  bool preload_enabled = false;
  String preload_header;
};

class IServiceWorkerPersistence {
 public:
  // The registrations that are worth keeping (those with a worker that is, or is about to be, active).
  virtual void snapshot(List<SavedRegistration>* out) = 0;
  // Brings one back with its worker active. False if there is no room.
  virtual bool restore(SavedRegistration&& saved) = 0;
  // Goes up with every change to a registration.
  virtual uint64_t version() = 0;

 protected:
  IServiceWorkerPersistence() = default;
  ~IServiceWorkerPersistence() = default;
};

}  // namespace browser
