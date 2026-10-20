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

#include "Browser/HttpPageLoader.hpp"

namespace browser {

bool HttpPageLoader::fetch(StringView address, PageResult* result) {
  network::INetworkStack* stack = nullptr;
  for (uint32_t i = 0; i < stacks_.count() && !stack; ++i)
    if (!stacks_.at(i)->configuration().address.is_unspecified()) stack = stacks_.at(i);
  if (!stack) {
    result->error.assign("There is no network connection.");
    return false;
  }
  network::HttpClient http(*stack, allocator_);
  if (has_tls_) http.set_tls(&tls_);
  http.set_max_body(kMaxPageBytes);
  http.add_header("User-Agent", "TheoryCraftOS-Browser/1.0");
  http.add_header("Accept", "text/html,text/plain;q=0.9,*/*;q=0.1");
  if (!http.start(network::HttpMethod::Get, address) || !http.run(timeout_ms_)) {
    result->error.assign(http.error() == network::HttpError::NoTlsConfiguration ? StringView("Secure (https) pages cannot be opened: no certificates are installed.")
                                                                                 : StringView(network::http_error_name(http.error())));
    return false;
  }
  result->status = http.status();
  http.url().assign_text(&result->final_address);
  result->content_type.assign(http.header("content-type"));
  result->body.clear();
  return result->body.append(http.body());
}

}  // namespace browser
