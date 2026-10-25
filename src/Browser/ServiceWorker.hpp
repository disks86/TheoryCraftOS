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

// Service workers. A registration (an origin, a scope and a script) lives in the browser, not in a page: it outlives the pages that made it and controls every page
// whose address is under its scope. Each worker of a registration walks the lifecycle of the standard - installing, installed (waiting), activating, activated,
// redundant - and runs in a Page of its own (no window shown; the script is the worker's global scope, set up by the service worker prelude), started when it is needed
// and stopped again when it has been idle. Everything runs on the thread that calls into the hub (a page, the desktop's loop), under one lock: a fetch event is handled
// from start to end inside the call that asked for it, the way the timers of the worker run ahead without waiting.
//
// The pages talk to the hub through natives (DomBindingsServiceWorker.cpp, __sw_*) and take what the hub wants them to hear - state changes, messages, a controller
// change - as events (take_client_event), which Page delivers to the script of navigator.serviceWorker.
#pragma once
#include "Browser/CacheStore.hpp"
#include "Browser/Fetcher.hpp"
#include "Browser/Page.hpp"
#include "Browser/ServiceWorkerTypes.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

class ServiceWorkerHub final : public IServiceWorkerPersistence {
 public:
  enum class Intercept : uint8_t { Pass, Responded, Failed };
  static constexpr uint32_t kMaxRegistrations = 64, kMaxWorkers = 256;

  using SavedRegistration = browser::SavedRegistration;
  struct Import {
    explicit Import(allocators::IAllocator& a) : url(a), source(a) {}
    String url, source;
  };

  explicit ServiceWorkerHub(allocators::IAllocator& allocator);
  ~ServiceWorkerHub();
  ServiceWorkerHub(const ServiceWorkerHub&) = delete;
  ServiceWorkerHub& operator=(const ServiceWorkerHub&) = delete;

  // The browser whose network, clock, cookies and storage the workers use. Must outlive the hub's workers (stop_all() before it goes). nullptr: no workers can run.
  void set_host(IPageEnvironment* host);
  bool available();
  // Stops every running worker (their pages are freed).
  void stop_all();
  // Everything the browser keeps for one origin / for all: registrations go (the workers become redundant).
  void clear_origin(StringView origin);
  void clear_all();

  // --- Clients: the documents of the browser
  // A document is shown at `url`; the registration whose scope contains it (and has an active worker) becomes its controller. Returns the client's id.
  uint32_t client_add(StringView url);
  void client_remove(uint32_t client);
  bool client_has_events(uint32_t client);
  // The next thing the hub has to tell the client, as JSON ({"t":"state"|"updatefound"|"controllerchange"|"message"|"navigate", ...}).
  bool client_take_event(uint32_t client, String* json);
  // The worker that controls the client (0: none); its description as JSON (for navigator.serviceWorker.controller).
  uint32_t client_controller(uint32_t client, String* json);   // (json may be nullptr)
  // Is there lifecycle work waiting (a worker to install or activate, a sync to fire)?
  bool busy();
  // Does a step of the lifecycle of every worker that is waiting for one; stops workers that have been idle for a while.
  void pump(uint64_t now_ms);

  // --- What navigator.serviceWorker does for a client (results are JSON; {"ok":false,"name":..,"message":..} for an error)
  void api_register(uint32_t client, StringView script_url, StringView scope, StringView type, StringView update_via_cache, String* json);
  void api_get_registration(uint32_t client, StringView url, String* json);
  void api_get_registrations(uint32_t client, String* json);
  void api_update(uint32_t reg, String* json);
  bool api_unregister(uint32_t reg);
  void api_registration(uint32_t reg, String* json);
  // postMessage(): to a worker, from a client (its id and the serialized message and its ports).
  bool api_post(uint32_t client, uint32_t worker, StringView wire, StringView ports);
  void api_preload(uint32_t reg, StringView op, StringView value, String* json);
  // Background sync: register a tag (fired as a `sync` event soon, the browser being online), the tags waiting.
  bool api_sync_register(uint32_t reg, StringView tag);
  void api_sync_tags(uint32_t reg, String* json);

  // --- Notifications and Push. The permission is kept per origin for as long as the browser runs; the notifications are shown by the browser (IPageEnvironment::notification_*).
  struct Permission {
    explicit Permission(allocators::IAllocator& a) : origin(a) {}
    String origin;
    bool granted = false;
  };
  static constexpr uint32_t kMaxPermissions = 256, kMaxNotifications = 64;
  // "default", "granted" or "denied" for the origin.
  const char* notification_permission(StringView origin);
  // The user's answer (or a setting): the pages of the origin are told ({"t":"permission"}) and the answers they wait for are given.
  void set_notification_permission(StringView origin, bool granted);
  // For a document (client) or a worker: its origin's permission; or ask for it (the answer is "pending" while the browser waits for the user).
  void notify_permission(uint32_t client, uint32_t worker, String* state);
  void notify_request(uint32_t client, uint32_t worker, String* state);
  // Shows a notification: from a document (reg 0) or for a registration. `options` is the JSON of the rest of the options, kept for the events; the names and texts of the action buttons are lines of `action_ids` / `action_titles`. {"ok":true,"id":N} or an error.
  void notify_show(uint32_t client, uint32_t reg, StringView title, StringView body, StringView tag, bool sticky, bool renotify, StringView options, StringView action_ids,
                   StringView action_titles, String* json);
  bool notify_close(uint32_t client, uint32_t id);
  // The notifications of a registration, as a JSON array (those with `tag` if it is not empty).
  void notify_list(uint32_t reg, StringView tag, String* json);
  // The user clicked / sent away the notice the browser showed under that number.
  // `action`: the button that was clicked (0 is the first), -1 for the notice itself; the worker's event says the action's name.
  void notification_activated(uint32_t number, int32_t action = -1);
  void notification_closed(uint32_t number);
  // Push: the subscription of a registration. The browser has no push service of its own to reach: a message is handed to push_deliver() with the endpoint of the subscription.
  void push_subscribe(uint32_t reg, bool user_visible_only, StringView key_hex, String* json);
  void push_get(uint32_t reg, String* json);   // the subscription as JSON, "" if there is none
  bool push_unsubscribe(uint32_t reg);
  void push_state(uint32_t reg, String* state);   // "granted", "denied" or "prompt"
  // The push service: the addresses of subscriptions' endpoints are on a host of its own, which the browser answers itself (an application server on this machine, or a page
  // acting as one, sends there). A POST with a TTL header and a body encrypted for the subscription (RFC 8291), signed with VAPID (RFC 8292) when the subscription was made
  // with an applicationServerKey, is decrypted and handed to the worker as a push event; the answer is the one of RFC 8030 (201, or 400, 401, 403, 404, 410, 413).
  static constexpr const char* kPushHost = "push.theorycraft.invalid";
  bool push_handles(StringView url);
  void push_request(const FetchOptions& options, NetResponse* response);
  // A message for the subscription: the worker gets a push event (with `data` when has_data). False if there is no such subscription or the worker did not take it.
  bool push_deliver(StringView endpoint, StringView data, bool has_data);
  // The subscription is gone on the push service's side: the worker gets pushsubscriptionchange.
  bool push_expire(StringView endpoint);

  // --- Message ports that cross between documents and workers (a port moved with postMessage() lives on the other side; the document or worker it left sends what is
  // posted to its peer here). Port numbers are unique over the whole browser (Page seeds them from base_port()).
  static uint32_t base_port(bool worker, uint32_t id) { return (worker ? 0x80000000u : 0u) + id * 65536u + 1u; }
  void port_message(uint32_t client, uint32_t worker, uint32_t port, StringView payload, StringView ports);

  // --- Fetch
  // The request of a document (client != 0), or a navigation to `options.url` (navigation true): if a service worker is in charge of it, its fetch event is dispatched.
  // Responded: *response is the answer. Failed: the worker answered with a network error (response->error says it). Pass: nobody took it, go to the network.
  Intercept intercept(uint32_t client, const FetchOptions& options, StringView destination, bool navigation, NetResponse* response);
  // Does a service worker with an active worker have this address in its scope (so that opening it is a controlled navigation)?
  bool controls(StringView url);

  // --- Saving
  uint64_t version() override;
  void snapshot(List<SavedRegistration>* out) override;
  bool restore(SavedRegistration&& saved) override;

  // --- Worker side: what the natives of a worker's global scope call (reached from inside a call of the hub, so the lock is held already)
  void worker_skip_waiting(uint32_t worker);
  void worker_claim(uint32_t worker);
  // The clients (documents) of the worker's origin as a JSON array of {id,url,type,frameType,visibilityState,focused,controlled}. `uncontrolled`: those not controlled by the
  // worker's registration too; `type` "window", "worker", "sharedworker" or "all".
  void worker_clients(uint32_t worker, bool uncontrolled, StringView type, String* json);
  bool worker_client(uint32_t worker, StringView id, String* json);
  bool worker_client_post(uint32_t worker, StringView id, StringView wire, StringView ports);
  bool worker_client_navigate(uint32_t worker, StringView id, StringView url, String* json);
  bool worker_open_window(uint32_t worker, StringView url);
  // The worker finished (or failed) what was dispatched to it under `token`; or answered a fetch event: status 0 with `kind` 2 is a network error, kind 1 a response, kind 0 none.
  void worker_done(uint32_t worker, uint32_t token, bool ok, StringView message);
  struct FetchAnswer {
    explicit FetchAnswer(allocators::IAllocator& a) : status_text(a), headers(a), body(a), url(a), type(a) {}
    uint32_t kind = 0;   // 0 the worker did not answer (go to the network), 1 a response, 2 a network error
    uint32_t status = 0;
    String status_text, headers, body, url, type;
    bool redirected = false;
  };
  void worker_answer(uint32_t worker, uint32_t token, FetchAnswer&& answer);
  // importScripts(): the source at `url` (from what the worker had when it was installed, else the network). False if it cannot be had.
  bool worker_import(uint32_t worker, StringView url, String* source);
  void worker_registration(uint32_t worker, String* json);
  void worker_update(uint32_t worker, String* json);
  bool worker_unregister(uint32_t worker);
  bool worker_sync_register(uint32_t worker, StringView tag);
  void worker_sync_tags(uint32_t worker, String* json);
  void worker_preload(uint32_t worker, StringView op, StringView value, String* json);

  struct Registration;
  struct Worker;
  struct Client;
  struct Runtime;
  struct Running;

 private:
  class Lock;
  struct Route {
    uint32_t port = 0, party = 0;
    bool to_worker = false;
  };
  List<Route> routes_;
  void set_route(uint32_t port, bool to_worker, uint32_t party);
  void route_ports(StringView csv, bool dest_worker, uint32_t dest, bool source_worker, uint32_t source);
  uint32_t next_token_ = 1;
  Registration* find_registration(uint32_t id);
  Worker* find_worker(uint32_t id);
  Client* find_client(uint32_t id);
  Registration* match_scope(StringView url);
  void registration_json(const Registration& r, String* json);
  void worker_json(const Worker& w, String* json);
  void emit_state(Registration& r, Worker& w);
  void emit_to_origin(StringView origin, StringView json);
  void emit_to_client(Client& c, StringView json);
  bool ensure_running(Worker& w);
  void stop_worker(Worker& w);
  bool run_to_completion(Worker& w, uint32_t token);
  void drain(Worker& w);
  bool dispatch_lifecycle(Worker& w, const char* type, String* message);
  bool dispatch_extendable(Worker& w, const char* type, StringView arg_json);
  bool fetch_script(Registration* existing, StringView origin, StringView script_url, StringView update_via_cache, String* source, String* final_url, String* allowed_scope, String* name,
                    String* message);
  bool new_worker(Registration& r, String&& source, const List<Import>* imports, Worker** out);
  bool evaluate(Worker& w, String* message);
  void make_redundant(Worker& w, bool notify);
  void remove_registration(Registration& r);
  void activate_worker(Registration& r, Worker& w);
  bool start_update(Registration& r, String* message, String* name, bool force_new);
  void claim_clients(Registration& r);
  void refresh_controllers();
  void fire_waiting_syncs(Registration& r);
  void touch() { ++version_; }
  struct Notification;
  Permission* find_permission(StringView origin);
  bool origin_of(uint32_t client, uint32_t worker, String* origin);
  void notification_json(const Notification& n, String* json);
  void remove_notification(uint64_t index, bool tell_host);
  void push_json(const Registration& r, String* json);
  void push_retire(Registration& r);
  void apply_permission(StringView origin, bool granted);

  allocators::IAllocator& allocator_;
  kernel::RecursiveMutex mutex_;
  IPageEnvironment* host_ = nullptr;
  uint32_t next_registration_ = 1, next_worker_ = 1, next_client_ = 1;
  uint64_t version_ = 0, now_ms_ = 0;
  List<UniquePointer<Registration>> registrations_;
  List<UniquePointer<Worker>> workers_;
  List<UniquePointer<Client>> clients_;
  List<Permission> permissions_;
  List<UniquePointer<Notification>> notifications_;
  uint32_t next_notification_ = 1;
  List<String> push_gone_;   // the tokens of endpoints that were given up (answered with 410)
  uint32_t push_messages_ = 0;
  friend struct Runtime;
};

}  // namespace browser
