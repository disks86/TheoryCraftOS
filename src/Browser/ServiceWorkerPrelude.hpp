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

// The script side of the Cache API and of service workers (ServiceWorkerPrelude.cpp): library code that runs in every page after the DOM prelude. The scope prelude
// runs besides that in the page that is the global scope of a service worker.
#pragma once

namespace browser {

const char* cache_prelude();                    // caches, Cache, CacheStorage
const char* service_worker_client_prelude();    // navigator.serviceWorker, ServiceWorkerRegistration, ServiceWorker
const char* service_worker_scope_prelude();     // ServiceWorkerGlobalScope: events, clients, skipWaiting, registration

}  // namespace browser
