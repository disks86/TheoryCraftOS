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

#include "Browser/ServiceWorker.hpp"
#include "Browser/WebPush.hpp"
#include "Browser/WebRandom.hpp"
#include "Crypto/Ecc.hpp"
#include "Browser/WebUrl.hpp"
#include "Utility/Move.hpp"

namespace browser {

namespace {

constexpr uint64_t kIdleStopMs = 30000, kUpdateIntervalMs = 24ull * 3600 * 1000;
constexpr uint32_t kRounds = 400, kMaxRedirects = 10, kMaxEvents = 256, kMaxRunning = 8;

// "text" as a JSON string (bytes of UTF-8 pass through).
void jq(String* out, StringView text) {
  out->push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          static const char kHex[] = "0123456789abcdef";
          out->append("\\u00");
          out->push_back(kHex[(static_cast<uint8_t>(c) >> 4) & 15]);
          out->push_back(kHex[static_cast<uint8_t>(c) & 15]);
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

void error_json(String* json, StringView name, StringView message) {
  json->clear();
  json->append("{\"ok\":false,\"name\":");
  jq(json, name);
  json->append(",\"message\":");
  jq(json, message);
  json->push_back('}');
}

bool is_script_type(StringView content_type) {
  StringView type = content_type;
  const uint64_t semi = type.find(';');
  if (semi != StringView::npos) type = type.first(semi);
  type = type.trimmed();
  static const char* const kTypes[] = {"application/javascript", "text/javascript", "application/x-javascript", "text/ecmascript", "application/ecmascript", "text/jscript",
                                       "text/x-javascript", "text/livescript", "application/x-ecmascript", "text/x-ecmascript", "application/jscript", "text/javascript1.0",
                                       "text/javascript1.1", "text/javascript1.2", "text/javascript1.3", "text/javascript1.4", "text/javascript1.5"};
  for (const char* t : kTypes)
    if (type.equals_ignore_case(t)) return true;
  return false;
}

// A secure context for a registration: https, or a local development host.
bool secure_origin(const WebUrl& url) {
  if (url.scheme.equals("https")) return true;
  if (!url.scheme.equals("http")) return false;
  const StringView host = url.host.view();
  return host.equals("localhost") || host.equals("127.0.0.1") || host.equals("[::1]") || host.ends_with(".localhost");
}

bool parse_url(allocators::IAllocator& a, StringView text, WebUrl* out) { return WebUrl::parse(a, text, nullptr, out); }

void without_fragment(allocators::IAllocator& a, StringView text, String* out) {
  WebUrl url(a);
  out->clear();
  if (WebUrl::parse(a, text, nullptr, &url)) url.serialize(out, false);
  else out->assign(text);
}

}  // namespace

// --- The data ---------------------------------------------------------------------------------------------------------------

struct ServiceWorkerHub::Worker {
  enum class State : uint8_t { Installing, Installed, Activating, Activated, Redundant };
  explicit Worker(allocators::IAllocator& a) : script_url(a), source(a), imports(a), message(a), answer(a) {}
  ~Worker();
  uint32_t id = 0, registration = 0;
  State state = State::Installing;
  String script_url, source;
  List<ServiceWorkerHub::Import> imports;
  UniquePointer<Runtime> runtime;
  uint64_t last_used_ms = 0;
  bool skip_waiting = false, install_started = false, evaluating = false, stop_pending = false;
  uint32_t depth = 0;   // calls into the worker's page that are running (it is not stopped under them)
  // what the dispatch in progress is waiting for
  uint32_t token = 0;
  bool done = false, ok = false, answered = false;
  String message;
  FetchAnswer answer;
};

struct ServiceWorkerHub::Registration {
  explicit Registration(allocators::IAllocator& a) : origin(a), scope(a), script_url(a), update_via_cache(a), type(a), preload_header(a), sync_tags(a), push_endpoint(a), push_p256dh(a), push_auth(a), push_key(a), push_private(a) {}
  uint32_t id = 0;
  String origin, scope, script_url, update_via_cache, type;
  String push_endpoint, push_p256dh, push_auth, push_key, push_private;   // the push subscription (endpoint empty: none); keys as hex
  uint32_t installing = 0, waiting = 0, active = 0;
  bool preload_enabled = false, unregistered = false, update_due = false;
  String preload_header;
  List<String> sync_tags;
  uint64_t last_update_ms = 0;
};

struct ServiceWorkerHub::Client {
  explicit Client(allocators::IAllocator& a) : url(a), origin(a), events(a) {}
  uint32_t id = 0, controller = 0;   // the registration that controls it
  String url, origin;
  List<String> events;
};

struct ServiceWorkerHub::Notification {
  explicit Notification(allocators::IAllocator& a) : origin(a), title(a), body(a), tag(a), options(a), actions(a) {}
  uint32_t id = 0, reg = 0, client = 0, number = 0;   // reg 0: made by a document (client); number: the browser's number for the notice
  String origin, title, body, tag, options, actions;   // actions: the names of the buttons, one on each line
};

// The environment a worker's page runs in: the browser's network, clock and storage, no window.
struct ServiceWorkerHub::Runtime final : IPageEnvironment {
  Runtime(ServiceWorkerHub& owner, uint32_t worker_id) : hub(owner), worker(worker_id), session(owner.allocator_) {}
  ~Runtime() { page.reset(); }
  ServiceWorkerHub& hub;
  uint32_t worker;
  StorageHub session;
  UniquePointer<Page> page;
  IPageEnvironment& host() { return *hub.host_; }
  bool fetch_text(StringView address, uint64_t limit, String* text) override {
    FetchOptions o;
    o.url = address;
    o.mode = FetchOptions::Mode::Subresource;
    o.now_ms = hub.host_->now_ms();
    NetResponse r(hub.allocator_);
    if (!hub.host_->http_request(o, &r) || r.status >= 400 || r.body.size() > limit) return false;
    return text->assign(StringView(r.body.view()));
  }
  CacheStore* cache_store() override { return hub.host_->cache_store(); }
  void navigate(StringView, bool) override {}
  void set_hash(StringView) override {}
  void reload() override {}
  void history_go(int32_t) override {}
  uint32_t history_length() override { return 1; }
  void push_state(StringView, StringView, bool, bool) override {}
  void alert(StringView) override {}
  bool confirm(StringView) override { return false; }
  void scroll_to(int32_t) override {}
  uint64_t now_ms() override { return hub.host_->now_ms(); }
  void console(int level, StringView text) override { hub.host_->console(level, text); }
  StorageHub& local_storage_hub() override { return hub.host_->local_storage_hub(); }
  StorageHub& session_storage_hub() override { return session; }
  PageHub& page_hub() override { return hub.host_->page_hub(); }
  uint64_t tab_id() override { return hub.host_->tab_id(); }
  CookieJar& cookies() override { return hub.host_->cookies(); }
  bool http_request(const FetchOptions& options, NetResponse* response) override { return hub.host_->http_request(options, response); }
  UniquePointer<IWebSocket> open_websocket(StringView url, StringView protocols, StringView origin, String* error) override { return hub.host_->open_websocket(url, protocols, origin, error); }
};

ServiceWorkerHub::Worker::~Worker() {}

// Marks a worker as running script for as long as it lives: stopping it waits until the outermost one is done.
struct ServiceWorkerHub::Running {
  explicit Running(Worker& worker) : w(worker) { ++w.depth; }
  ~Running() {
    if (--w.depth == 0 && w.stop_pending) {
      w.stop_pending = false;
      w.runtime.reset();
    }
  }
  Worker& w;
};

class ServiceWorkerHub::Lock {
 public:
  explicit Lock(ServiceWorkerHub& hub) : mutex_(hub.mutex_), held_(true) { mutex_.lock(); }
  // For the questions every page asks often: if a worker is busy answering someone (the lock is held, perhaps for as long as a network request), the answer is "nothing".
  Lock(ServiceWorkerHub& hub, bool) : mutex_(hub.mutex_), held_(hub.mutex_.try_lock()) {}
  ~Lock() {
    if (held_) mutex_.unlock();
  }
  bool held() const { return held_; }

 private:
  kernel::RecursiveMutex& mutex_;
  bool held_;
};

ServiceWorkerHub::ServiceWorkerHub(allocators::IAllocator& allocator) : allocator_(allocator), routes_(allocator), registrations_(allocator), workers_(allocator), clients_(allocator), permissions_(allocator), notifications_(allocator), push_gone_(allocator) {}

ServiceWorkerHub::~ServiceWorkerHub() { stop_all(); }

void ServiceWorkerHub::set_host(IPageEnvironment* host) {
  Lock lock(*this);
  host_ = host;
}

bool ServiceWorkerHub::available() {
  Lock lock(*this);
  return host_ != nullptr;
}

void ServiceWorkerHub::stop_all() {
  Lock lock(*this);
  for (uint64_t i = 0; i < workers_.count(); ++i) workers_[i]->runtime.reset();
}

ServiceWorkerHub::Registration* ServiceWorkerHub::find_registration(uint32_t id) {
  for (uint64_t i = 0; i < registrations_.count(); ++i)
    if (registrations_[i]->id == id) return registrations_[i].get();
  return nullptr;
}

ServiceWorkerHub::Worker* ServiceWorkerHub::find_worker(uint32_t id) {
  for (uint64_t i = 0; i < workers_.count(); ++i)
    if (workers_[i]->id == id) return workers_[i].get();
  return nullptr;
}

ServiceWorkerHub::Client* ServiceWorkerHub::find_client(uint32_t id) {
  for (uint64_t i = 0; i < clients_.count(); ++i)
    if (clients_[i]->id == id) return clients_[i].get();
  return nullptr;
}

// The registration with the longest scope that holds `url` and has an active worker (the one in charge of a page at that address).
ServiceWorkerHub::Registration* ServiceWorkerHub::match_scope(StringView url) {
  String plain(allocator_);
  without_fragment(allocator_, url, &plain);
  Registration* best = nullptr;
  for (uint64_t i = 0; i < registrations_.count(); ++i) {
    Registration& r = *registrations_[i];
    if (r.unregistered || r.active == 0) continue;
    Worker* w = find_worker(r.active);
    if (!w || w->state != Worker::State::Activated) continue;
    if (!plain.view().starts_with(r.scope.view())) continue;
    if (!best || r.scope.size() > best->scope.size()) best = &r;
  }
  return best;
}

// --- JSON for the pages -------------------------------------------------------------------------------------------------------

static const char* state_name(ServiceWorkerHub::Worker::State s) {
  switch (s) {
    case ServiceWorkerHub::Worker::State::Installing: return "installing";
    case ServiceWorkerHub::Worker::State::Installed: return "installed";
    case ServiceWorkerHub::Worker::State::Activating: return "activating";
    case ServiceWorkerHub::Worker::State::Activated: return "activated";
    case ServiceWorkerHub::Worker::State::Redundant: return "redundant";
  }
  return "redundant";
}

void ServiceWorkerHub::worker_json(const Worker& w, String* json) {
  json->append("{\"id\":");
  json->append_uint(w.id);
  json->append(",\"reg\":");
  json->append_uint(w.registration);
  json->append(",\"scriptURL\":");
  jq(json, w.script_url.view());
  json->append(",\"state\":");
  jq(json, state_name(w.state));
  json->push_back('}');
}

void ServiceWorkerHub::registration_json(const Registration& r, String* json) {
  json->append("{\"id\":");
  json->append_uint(r.id);
  json->append(",\"scope\":");
  jq(json, r.scope.view());
  json->append(",\"scriptURL\":");
  jq(json, r.script_url.view());
  json->append(",\"updateViaCache\":");
  jq(json, r.update_via_cache.view());
  json->append(",\"type\":");
  jq(json, r.type.view());
  const uint32_t slots[3] = {r.installing, r.waiting, r.active};
  const char* names[3] = {"installing", "waiting", "active"};
  for (int i = 0; i < 3; ++i) {
    json->append(",\"");
    json->append(names[i]);
    json->append("\":");
    Worker* w = slots[i] ? find_worker(slots[i]) : nullptr;
    if (w) worker_json(*w, json);
    else json->append("null");
  }
  json->append(",\"preload\":{\"enabled\":");
  json->append(r.preload_enabled ? "true" : "false");
  json->append(",\"header\":");
  jq(json, r.preload_header.view());
  json->append("}}");
}

// --- Events for the pages -------------------------------------------------------------------------------------------------------

void ServiceWorkerHub::emit_to_client(Client& c, StringView json) {
  if (c.events.count() >= kMaxEvents) c.events.remove_at(0);
  String text(allocator_);
  text.assign(json);
  c.events.push_back(util::move(text));
}

void ServiceWorkerHub::emit_to_origin(StringView origin, StringView json) {
  for (uint64_t i = 0; i < clients_.count(); ++i)
    if (clients_[i]->origin.view().equals(origin)) emit_to_client(*clients_[i], json);
}

void ServiceWorkerHub::emit_state(Registration& r, Worker& w) {
  String json(allocator_);
  json.append("{\"t\":\"state\",\"worker\":");
  json.append_uint(w.id);
  json.append(",\"state\":");
  jq(&json, state_name(w.state));
  json.append(",\"reg\":");
  registration_json(r, &json);
  json.push_back('}');
  emit_to_origin(r.origin.view(), json.view());
  touch();
}

bool ServiceWorkerHub::client_has_events(uint32_t client) {
  Lock lock(*this, true);
  if (!lock.held()) return false;
  Client* c = find_client(client);
  return c && c->events.count() > 0;
}

bool ServiceWorkerHub::client_take_event(uint32_t client, String* json) {
  Lock lock(*this, true);
  if (!lock.held()) return false;
  Client* c = find_client(client);
  if (!c || c->events.count() == 0) return false;
  json->assign(c->events[0].view());
  c->events.remove_at(0);
  return true;
}

uint32_t ServiceWorkerHub::client_add(StringView url) {
  Lock lock(*this);
  if (!host_) return 0;
  auto made = UniquePointer<Client>::make(allocator_, allocator_);
  if (!made) return 0;
  made->id = next_client_++;
  without_fragment(allocator_, url, &made->url);
  WebUrl parsed(allocator_);
  if (parse_url(allocator_, url, &parsed)) parsed.origin(&made->origin);
  else made->origin.assign("null");
  if (Registration* r = match_scope(url)) {
    made->controller = r->id;
    if (host_->now_ms() - r->last_update_ms > kUpdateIntervalMs) r->update_due = true;   // (a controlled page loaded: time for a look at the script)
  }
  const uint32_t id = made->id;
  if (!clients_.push_back(util::move(made))) return 0;
  return id;
}

void ServiceWorkerHub::client_remove(uint32_t client) {
  Lock lock(*this);
  for (uint64_t i = notifications_.count(); i-- > 0;)   // (the notices of a document go with it)
    if (notifications_[i]->reg == 0 && notifications_[i]->client == client) remove_notification(i, true);
  for (uint64_t i = 0; i < clients_.count(); ++i)
    if (clients_[i]->id == client) {
      clients_.remove_at(i);
      return;
    }
}

uint32_t ServiceWorkerHub::client_controller(uint32_t client, String* json) {
  Lock lock(*this);
  Client* c = find_client(client);
  if (!c || c->controller == 0) return 0;
  Registration* r = find_registration(c->controller);
  Worker* w = r && r->active ? find_worker(r->active) : nullptr;
  if (!w) return 0;
  if (json) worker_json(*w, json);
  return w->id;
}

// --- Running a worker ---------------------------------------------------------------------------------------------------------

bool ServiceWorkerHub::ensure_running(Worker& w) {
  if (w.runtime) {
    w.last_used_ms = host_ ? host_->now_ms() : 0;
    return true;
  }
  if (!host_ || w.state == Worker::State::Redundant) return false;
  for (;;) {   // (a few workers run at a time: the one idle the longest makes room)
    uint32_t running = 0;
    Worker* oldest = nullptr;
    for (uint64_t i = 0; i < workers_.count(); ++i)
      if (workers_[i]->runtime && workers_[i].get() != &w) {
        ++running;
        if (workers_[i]->depth == 0 && (!oldest || workers_[i]->last_used_ms < oldest->last_used_ms)) oldest = workers_[i].get();
      }
    if (running < kMaxRunning || !oldest) break;
    stop_worker(*oldest);
  }
  auto runtime = UniquePointer<Runtime>::make(allocator_, *this, w.id);
  if (!runtime) return false;
  MediaEnvironment media;
  runtime->page = UniquePointer<Page>::make(allocator_, allocator_, *runtime, w.script_url.view(), media, true);
  if (!runtime->page) return false;
  runtime->page->set_service_worker(this, w.id);
  runtime->page->load("<!doctype html><html><head></head><body></body></html>");
  w.last_used_ms = host_->now_ms();
  String error(allocator_);
  w.runtime = util::move(runtime);
  Running running(w);
  w.evaluating = true;
  const bool ok = w.runtime->page->run_worker_script(w.source.view(), &error);
  w.evaluating = false;
  if (!ok || w.stop_pending) {
    w.message.assign(error.view());
    stop_worker(w);
    return false;
  }
  return true;
}

void ServiceWorkerHub::stop_worker(Worker& w) {
  if (w.depth > 0) w.stop_pending = true;
  else w.runtime.reset();
}

// Runs the worker's timers (the network among them) until what was dispatched under `token` has settled, the worker has nothing left to run, or a round limit.
bool ServiceWorkerHub::run_to_completion(Worker& w, uint32_t token) {
  for (uint32_t i = 0; i < kRounds && w.runtime; ++i) {
    if (w.done && w.token == token) return true;
    if (!w.runtime->page->run_timers_ahead(64)) break;
  }
  return w.done && w.token == token;
}

// What is left to run after the worker answered (waitUntil work: a cache write after the response went out).
void ServiceWorkerHub::drain(Worker& w) {
  for (uint32_t i = 0; i < 64 && w.runtime; ++i)
    if (!w.runtime->page->run_timers_ahead(64)) break;
}

// install / activate: the event is dispatched and the worker given time to settle its waitUntil() promises.
bool ServiceWorkerHub::dispatch_lifecycle(Worker& w, const char* type, String* message) {
  if (!ensure_running(w)) {
    message->assign(w.message.empty() ? StringView("The service worker could not be started.") : w.message.view());
    return false;
  }
  Running running(w);
  w.token = next_token_++;
  w.done = false;
  w.ok = false;
  w.message.clear();
  using Arg = DomBindings::HookArg;
  const Arg args[2] = {Arg::of(StringView(type)), Arg::of(static_cast<double>(w.token))};
  if (!w.runtime->page->call_global("__sw_dispatch", args, 2)) {
    message->assign("The event could not be dispatched.");
    return false;
  }
  const uint32_t token = w.token;
  const bool settled = run_to_completion(w, token);
  if (!settled) {
    message->assign("The service worker did not finish in time.");
    return false;
  }
  message->assign(w.message.view());
  const bool ok = w.ok;
  if (w.runtime) drain(w);
  return ok;
}

// An event with an argument (message, sync ...): dispatched, waited for, the outcome ignored.
bool ServiceWorkerHub::dispatch_extendable(Worker& w, const char* type, StringView arg_json) {
  if (!ensure_running(w)) return false;
  Running running(w);
  w.token = next_token_++;
  w.done = false;
  w.ok = false;
  w.message.clear();
  using Arg = DomBindings::HookArg;
  const Arg args[3] = {Arg::of(StringView(type)), Arg::of(static_cast<double>(w.token)), Arg::of(arg_json)};
  if (!w.runtime->page->call_global("__sw_dispatch", args, 3)) return false;
  const uint32_t token = w.token;
  const bool settled = run_to_completion(w, token);
  const bool ok = settled && w.ok;
  if (w.runtime) drain(w);
  return ok;
}

void ServiceWorkerHub::worker_done(uint32_t worker, uint32_t token, bool ok, StringView message) {
  Worker* w = find_worker(worker);
  if (!w || w->token != token) return;
  w->done = true;
  w->ok = ok;
  w->message.assign(message);
}

void ServiceWorkerHub::worker_answer(uint32_t worker, uint32_t token, FetchAnswer&& answer) {
  Worker* w = find_worker(worker);
  if (!w || w->token != token) return;
  w->answer = util::move(answer);
  w->answered = true;
  w->done = true;
  w->ok = true;
}

bool ServiceWorkerHub::worker_import(uint32_t worker, StringView url, String* source) {
  Worker* w = find_worker(worker);
  if (!w || !host_) return false;
  String plain(allocator_);
  without_fragment(allocator_, url, &plain);
  for (const Import& i : w->imports)
    if (i.url.view().equals(plain.view())) return source->assign(i.source.view());
  if (!w->evaluating && w->state != Worker::State::Installing) return false;   // (only the first run of a worker may fetch more: the rest is what it was installed with)
  FetchOptions o;
  o.url = plain.view();
  o.mode = FetchOptions::Mode::Subresource;
  o.now_ms = host_->now_ms();
  NetResponse r(allocator_);
  if (!host_->http_request(o, &r) || r.status != 200) return false;
  Import import(allocator_);
  import.url.assign(plain.view());
  import.source.assign(StringView(r.body.view()));
  source->assign(import.source.view());
  w->imports.push_back(util::move(import));
  return true;
}

// --- Workers and registrations -----------------------------------------------------------------------------------------------

bool ServiceWorkerHub::new_worker(Registration& r, String&& source, const List<Import>* imports, Worker** out) {
  if (workers_.count() >= kMaxWorkers) {
    for (uint64_t i = 0; i < workers_.count();)   // (redundant workers nobody needs go first)
      if (workers_[i]->state == Worker::State::Redundant) workers_.remove_at(i);
      else ++i;
  }
  auto made = UniquePointer<Worker>::make(allocator_, allocator_);
  if (!made) return false;
  made->id = next_worker_++;
  made->registration = r.id;
  made->script_url.assign(r.script_url.view());
  made->source = util::move(source);
  if (imports)
    for (const Import& i : *imports) {
      Import copy(allocator_);
      copy.url.assign(i.url.view());
      copy.source.assign(i.source.view());
      made->imports.push_back(util::move(copy));
    }
  *out = made.get();
  return workers_.push_back(util::move(made));
}

// The worker is of no more use: its slot in the registration is emptied and its page stopped.
void ServiceWorkerHub::make_redundant(Worker& w, bool notify) {
  if (w.state == Worker::State::Redundant) return;
  w.state = Worker::State::Redundant;
  stop_worker(w);
  Registration* r = find_registration(w.registration);
  if (!r) return;
  if (r->installing == w.id) r->installing = 0;
  if (r->waiting == w.id) r->waiting = 0;
  if (r->active == w.id) r->active = 0;
  if (notify) emit_state(*r, w);
}

void ServiceWorkerHub::remove_registration(Registration& r) {
  const uint32_t id = r.id;
  for (uint64_t i = 0; i < workers_.count(); ++i)
    if (workers_[i]->registration == id) {
      if (workers_[i]->state != Worker::State::Redundant) {
        workers_[i]->state = Worker::State::Redundant;
        stop_worker(*workers_[i]);
      }
    }
  for (uint64_t i = 0; i < clients_.count(); ++i)
    if (clients_[i]->controller == id) clients_[i]->controller = 0;
  for (uint64_t i = 0; i < registrations_.count(); ++i)
    if (registrations_[i]->id == id) {
      registrations_.remove_at(i);
      break;
    }
  touch();
}

// Fetches a worker script. On failure *name and *message say why (the errors register() throws).
bool ServiceWorkerHub::fetch_script(Registration* existing, StringView origin, StringView script_url, StringView update_via_cache, String* source, String* final_url, String* allowed_scope,
                                    String* name, String* message) {
  (void)existing;
  (void)update_via_cache;
  FetchOptions o;
  o.url = script_url;
  o.mode = FetchOptions::Mode::Subresource;
  o.headers = "Service-Worker: script\r\n";
  o.page_address = origin;
  o.now_ms = host_->now_ms();
  NetResponse r(allocator_);
  if (!host_->http_request(o, &r)) {
    name->assign("TypeError");
    message->assign("An unknown error occurred when fetching the script.");
    return false;
  }
  if (r.status != 200) {
    name->assign("TypeError");
    message->assign("A bad HTTP response code (");
    message->append_uint(r.status);
    message->append(") was received when fetching the script.");
    return false;
  }
  StringView content_type;
  header_value(r.headers.view(), "content-type", &content_type);
  if (!is_script_type(content_type)) {
    name->assign("SecurityError");
    message->assign("The script has an unsupported MIME type ('");
    message->append(content_type);
    message->append("').");
    return false;
  }
  StringView allowed;
  allowed_scope->clear();
  if (header_value(r.headers.view(), "service-worker-allowed", &allowed)) {
    WebUrl base(allocator_), resolved(allocator_);
    if (parse_url(allocator_, script_url, &base) && WebUrl::parse(allocator_, allowed, &base, &resolved)) resolved.serialize(allowed_scope, false);
  }
  final_url->assign(r.url.empty() ? script_url : r.url.view());
  source->assign(StringView(r.body.view()));
  return true;
}

bool ServiceWorkerHub::evaluate(Worker& w, String* message) {
  if (ensure_running(w)) return true;
  message->assign(w.message.view());
  return false;
}

// register(): the checks of the standard, the script fetched, a registration made (or found) and a new worker installing in it.
void ServiceWorkerHub::api_register(uint32_t client, StringView script_url, StringView scope, StringView type, StringView update_via_cache, String* json) {
  Lock lock(*this);
  Client* c = find_client(client);
  if (!host_ || !c) {
    error_json(json, "InvalidStateError", "Failed to register a ServiceWorker: The document is in an invalid state.");
    return;
  }
  WebUrl script(allocator_), scope_url(allocator_), page(allocator_);
  String script_text(allocator_), scope_text(allocator_), script_origin(allocator_), scope_origin(allocator_), message(allocator_), name(allocator_);
  if (!parse_url(allocator_, script_url, &script) || !parse_url(allocator_, scope, &scope_url) || !parse_url(allocator_, c->url.view(), &page)) {
    error_json(json, "TypeError", "Failed to register a ServiceWorker: The provided scriptURL or scope is invalid.");
    return;
  }
  script.serialize(&script_text, false);
  scope_url.serialize(&scope_text, false);
  script.origin(&script_origin);
  scope_url.origin(&scope_origin);
  auto describe = [&](const char* why) {
    message.assign("Failed to register a ServiceWorker for scope ('");
    message.append(scope_text.view());
    message.append("') with script ('");
    message.append(script_text.view());
    message.append("'): ");
    message.append(why);
  };
  if (!secure_origin(page)) {
    describe("The URL protocol of the current origin ('");
    message.append(c->origin.view());
    message.append("') is not supported.");
    error_json(json, "SecurityError", message.view());
    return;
  }
  if (!script_origin.view().equals(c->origin.view())) {
    message.assign("Failed to register a ServiceWorker: The origin of the provided scriptURL ('");
    message.append(script_origin.view());
    message.append("') does not match the current origin ('");
    message.append(c->origin.view());
    message.append("').");
    error_json(json, "SecurityError", message.view());
    return;
  }
  if (!scope_origin.view().equals(c->origin.view())) {
    message.assign("Failed to register a ServiceWorker: The origin of the provided scope ('");
    message.append(scope_origin.view());
    message.append("') does not match the current origin ('");
    message.append(c->origin.view());
    message.append("').");
    error_json(json, "SecurityError", message.view());
    return;
  }
  const StringView path = script.path.view();
  if (path.contains("%2f") || path.contains("%2F") || path.contains("%5c") || path.contains("%5C")) {
    describe("The provided scriptURL ('");
    message.append(script_text.view());
    message.append("') includes a disallowed escape sequence.");
    error_json(json, "TypeError", message.view());
    return;
  }
  String source(allocator_), final_url(allocator_), allowed(allocator_);
  if (!fetch_script(nullptr, c->origin.view(), script_text.view(), update_via_cache, &source, &final_url, &allowed, &name, &message)) {
    String full(allocator_);
    full.assign("Failed to register a ServiceWorker for scope ('");
    full.append(scope_text.view());
    full.append("') with script ('");
    full.append(script_text.view());
    full.append("'): ");
    full.append(message.view());
    error_json(json, name.view(), full.view());
    return;
  }
  // the widest scope the script may have: its directory, or what Service-Worker-Allowed says
  String max_scope(allocator_);
  if (!allowed.empty()) {
    max_scope.assign(allowed.view());
  } else {
    WebUrl dir(allocator_);
    WebUrl::parse(allocator_, ".", &script, &dir);
    dir.serialize(&max_scope, false);
  }
  WebUrl max_url(allocator_);
  parse_url(allocator_, max_scope.view(), &max_url);
  if (!scope_url.path.view().starts_with(max_url.path.view())) {
    describe("The path of the provided scope ('");
    message.append(scope_url.path.view());
    message.append("') is not under the max scope allowed ('");
    message.append(max_url.path.view());
    message.append("'). Adjust the scope, move the Service Worker script, or use the Service-Worker-Allowed HTTP header to allow the scope.");
    error_json(json, "SecurityError", message.view());
    return;
  }
  Registration* reg = nullptr;
  for (uint64_t i = 0; i < registrations_.count(); ++i)
    if (!registrations_[i]->unregistered && registrations_[i]->origin.view().equals(c->origin.view()) && registrations_[i]->scope.view().equals(scope_text.view())) reg = registrations_[i].get();
  bool created = false;
  if (reg && reg->script_url.view().equals(script_text.view())) {   // the same script for the same scope: that registration, looked at again
    const uint32_t newest = reg->installing ? reg->installing : reg->waiting ? reg->waiting : reg->active;
    Worker* current = newest ? find_worker(newest) : nullptr;
    if (current && !current->source.view().equals(source.view())) {
      reg->update_via_cache.assign(update_via_cache);
    } else if (current) {
      json->assign("{\"ok\":true,\"reg\":");
      registration_json(*reg, json);
      json->push_back('}');
      return;
    }
  }
  if (!reg) {
    if (registrations_.count() >= kMaxRegistrations) {
      error_json(json, "QuotaExceededError", "Failed to register a ServiceWorker: Too many registrations.");
      return;
    }
    auto made = UniquePointer<Registration>::make(allocator_, allocator_);
    if (!made) {
      error_json(json, "TypeError", "Failed to register a ServiceWorker: Out of memory.");
      return;
    }
    made->id = next_registration_++;
    made->origin.assign(c->origin.view());
    made->scope.assign(scope_text.view());
    reg = made.get();
    registrations_.push_back(util::move(made));
    created = true;
  }
  reg->script_url.assign(script_text.view());
  reg->update_via_cache.assign(update_via_cache.empty() ? StringView("imports") : update_via_cache);
  reg->type.assign(type.empty() ? StringView("classic") : type);
  reg->last_update_ms = host_->now_ms();
  Worker* w = nullptr;
  if (!new_worker(*reg, util::move(source), nullptr, &w)) {
    if (created) remove_registration(*reg);
    error_json(json, "TypeError", "Failed to register a ServiceWorker: Out of memory.");
    return;
  }
  // the script is run once now: a script that does not compile or throws on its top level fails the registration
  if (!evaluate(*w, &message)) {
    w->state = Worker::State::Redundant;
    if (created) remove_registration(*reg);
    String full(allocator_);
    full.assign("Failed to register a ServiceWorker for scope ('");
    full.append(scope_text.view());
    full.append("') with script ('");
    full.append(script_text.view());
    full.append("'): ServiceWorker script evaluation failed");
    error_json(json, "TypeError", full.view());
    return;
  }
  if (reg->installing) {
    if (Worker* old = find_worker(reg->installing)) make_redundant(*old, true);
  }
  reg->installing = w->id;
  touch();
  String event(allocator_);
  event.assign("{\"t\":\"updatefound\",\"reg\":");
  registration_json(*reg, &event);
  event.push_back('}');
  emit_to_origin(reg->origin.view(), event.view());
  json->assign("{\"ok\":true,\"reg\":");
  registration_json(*reg, json);
  json->push_back('}');
}

void ServiceWorkerHub::api_get_registration(uint32_t client, StringView url, String* json) {
  Lock lock(*this);
  Client* c = find_client(client);
  json->clear();
  if (!c) return;
  WebUrl base(allocator_), target(allocator_);
  parse_url(allocator_, c->url.view(), &base);
  String text(allocator_);
  if (url.empty()) text.assign(c->url.view());
  else if (WebUrl::parse(allocator_, url, &base, &target)) target.serialize(&text, false);
  else return;
  Registration* best = nullptr;
  for (uint64_t i = 0; i < registrations_.count(); ++i) {
    Registration& r = *registrations_[i];
    if (r.unregistered || !r.origin.view().equals(c->origin.view()) || !text.view().starts_with(r.scope.view())) continue;
    if (!best || r.scope.size() > best->scope.size()) best = &r;
  }
  if (best) registration_json(*best, json);
}

void ServiceWorkerHub::api_get_registrations(uint32_t client, String* json) {
  Lock lock(*this);
  Client* c = find_client(client);
  json->assign("[");
  bool first = true;
  for (uint64_t i = 0; c && i < registrations_.count(); ++i) {
    const Registration& r = *registrations_[i];
    if (r.unregistered || !r.origin.view().equals(c->origin.view())) continue;
    if (!first) json->push_back(',');
    first = false;
    registration_json(r, json);
  }
  json->push_back(']');
}

void ServiceWorkerHub::api_registration(uint32_t reg, String* json) {
  Lock lock(*this);
  json->clear();
  if (Registration* r = find_registration(reg)) registration_json(*r, json);
}

// update(): the script is fetched again; a different one (or different imports) starts a new worker.
bool ServiceWorkerHub::start_update(Registration& r, String* message, String* name, bool force_new) {
  String source(allocator_), final_url(allocator_), allowed(allocator_);
  r.last_update_ms = host_->now_ms();
  r.update_due = false;
  if (!fetch_script(&r, r.origin.view(), r.script_url.view(), r.update_via_cache.view(), &source, &final_url, &allowed, name, message)) return false;
  const uint32_t newest = r.installing ? r.installing : r.waiting ? r.waiting : r.active;
  Worker* current = newest ? find_worker(newest) : nullptr;
  if (!force_new && current && current->source.view().equals(source.view())) {   // (imported scripts are compared too: fetched again, they must be the same)
    bool same = true;
    for (const Import& i : current->imports) {
      FetchOptions o;
      o.url = i.url.view();
      o.mode = FetchOptions::Mode::Subresource;
      o.now_ms = host_->now_ms();
      NetResponse nr(allocator_);
      if (host_->http_request(o, &nr) && nr.status == 200 && !StringView(nr.body.view()).equals(i.source.view())) same = false;
    }
    if (same) return true;
  }
  Worker* w = nullptr;
  if (!new_worker(r, util::move(source), nullptr, &w)) {
    name->assign("TypeError");
    message->assign("Out of memory.");
    return false;
  }
  if (!evaluate(*w, message)) {
    w->state = Worker::State::Redundant;
    name->assign("TypeError");
    message->assign("ServiceWorker script evaluation failed");
    return false;
  }
  if (r.installing) {
    if (Worker* old = find_worker(r.installing)) make_redundant(*old, true);
  }
  r.installing = w->id;
  touch();
  String event(allocator_);
  event.assign("{\"t\":\"updatefound\",\"reg\":");
  registration_json(r, &event);
  event.push_back('}');
  emit_to_origin(r.origin.view(), event.view());
  return true;
}

void ServiceWorkerHub::api_update(uint32_t reg, String* json) {
  Lock lock(*this);
  Registration* r = find_registration(reg);
  if (!host_ || !r || r->unregistered) {
    error_json(json, "InvalidStateError", "Failed to update a ServiceWorker: The registration is no longer active.");
    return;
  }
  String message(allocator_), name(allocator_);
  if (!start_update(*r, &message, &name, false)) {
    String full(allocator_);
    full.assign("Failed to update a ServiceWorker for scope ('");
    full.append(r->scope.view());
    full.append("') with script ('");
    full.append(r->script_url.view());
    full.append("'): ");
    full.append(message.view());
    error_json(json, name.view(), full.view());
    return;
  }
  json->assign("{\"ok\":true,\"reg\":");
  registration_json(*r, json);
  json->push_back('}');
}

// unregister(): the registration is gone for lookups at once; its workers go on serving the documents they control until those are closed.
bool ServiceWorkerHub::api_unregister(uint32_t reg) {
  Lock lock(*this);
  Registration* r = find_registration(reg);
  if (!r || r->unregistered) return false;
  r->unregistered = true;
  if (r->installing) if (Worker* w = find_worker(r->installing)) make_redundant(*w, true);
  if (r->waiting) if (Worker* w = find_worker(r->waiting)) make_redundant(*w, true);
  touch();
  return true;
}

void ServiceWorkerHub::set_route(uint32_t port, bool to_worker, uint32_t party) {
  for (uint64_t i = 0; i < routes_.count(); ++i)
    if (routes_[i].port == port) {
      routes_[i].to_worker = to_worker;
      routes_[i].party = party;
      return;
    }
  if (routes_.count() >= 4096) routes_.remove_at(0);
  Route r;
  r.port = port;
  r.to_worker = to_worker;
  r.party = party;
  routes_.push_back(r);
}

// Ports moved to `dest` (a worker or a document): what is posted to them goes there, what is posted to their peers goes back to the sender.
void ServiceWorkerHub::route_ports(StringView csv, bool dest_worker, uint32_t dest, bool source_worker, uint32_t source) {
  StringView rest = csv, token;
  while (rest.next_token(',', &token)) {
    uint32_t id = 0;
    bool ok = !token.empty();
    for (const char ch : token) {
      if (ch < '0' || ch > '9') ok = false;
      else id = id * 10 + static_cast<uint32_t>(ch - '0');
    }
    if (!ok || id == 0) continue;
    set_route(id, dest_worker, dest);
    set_route(id % 2 == 1 ? id + 1 : id - 1, source_worker, source);
  }
}

void ServiceWorkerHub::port_message(uint32_t client, uint32_t worker, uint32_t port, StringView payload, StringView ports) {
  Lock lock(*this);
  const Route* found = nullptr;
  for (uint64_t i = 0; i < routes_.count(); ++i)
    if (routes_[i].port == port) found = &routes_[i];
  if (!found) return;
  const bool to_worker = found->to_worker;
  const uint32_t party = found->party;
  route_ports(ports, to_worker, party, worker != 0, worker != 0 ? worker : client);
  if (to_worker) {
    Worker* w = find_worker(party);
    if (!w || !ensure_running(*w)) return;
    Running running(*w);
    using Arg = DomBindings::HookArg;
    const Arg args[3] = {Arg::of(static_cast<double>(port)), Arg::of(payload), Arg::of(ports)};
    w->runtime->page->call_global("__port_deliver", args, 3);
    drain(*w);
  } else if (Client* c = find_client(party)) {
    String event(allocator_);
    event.append("{\"t\":\"port\",\"port\":");
    event.append_uint(port);
    event.append(",\"data\":");
    jq(&event, payload);
    event.append(",\"ports\":");
    jq(&event, ports);
    event.push_back('}');
    emit_to_client(*c, event.view());
  }
}

bool ServiceWorkerHub::api_post(uint32_t client, uint32_t worker, StringView wire, StringView ports) {
  Lock lock(*this);
  Worker* w = find_worker(worker);
  Client* c = find_client(client);
  if (!w || !c || w->state == Worker::State::Redundant) return false;
  route_ports(ports, true, worker, false, client);
  String arg(allocator_);
  arg.append("{\"data\":");
  jq(&arg, wire);
  arg.append(",\"ports\":");
  jq(&arg, ports);
  arg.append(",\"source\":");
  String id(allocator_);
  id.append_uint(c->id);
  jq(&arg, id.view());
  arg.append(",\"origin\":");
  jq(&arg, c->origin.view());
  arg.push_back('}');
  return dispatch_extendable(*w, "message", arg.view());
}

void ServiceWorkerHub::api_preload(uint32_t reg, StringView op, StringView value, String* json) {
  Lock lock(*this);
  Registration* r = find_registration(reg);
  json->clear();
  if (!r) return;
  if (op.equals("enable")) r->preload_enabled = true;
  else if (op.equals("disable")) r->preload_enabled = false;
  else if (op.equals("header")) r->preload_header.assign(value);
  touch();
  json->append("{\"enabled\":");
  json->append(r->preload_enabled ? "true" : "false");
  json->append(",\"header\":");
  jq(json, r->preload_header.empty() ? StringView("true") : r->preload_header.view());
  json->push_back('}');
}

bool ServiceWorkerHub::api_sync_register(uint32_t reg, StringView tag) {
  Lock lock(*this);
  Registration* r = find_registration(reg);
  if (!r || r->unregistered || r->active == 0) return false;
  for (const String& t : r->sync_tags)
    if (t.view().equals(tag)) return true;
  String copy(allocator_);
  copy.assign(tag);
  return r->sync_tags.push_back(util::move(copy));
}

void ServiceWorkerHub::api_sync_tags(uint32_t reg, String* json) {
  Lock lock(*this);
  Registration* r = find_registration(reg);
  json->assign("[");
  for (uint64_t i = 0; r && i < r->sync_tags.count(); ++i) {
    if (i) json->push_back(',');
    jq(json, r->sync_tags[i].view());
  }
  json->push_back(']');
}

// --- Notifications and Push ------------------------------------------------------------------------------------------------------

namespace {

void hex_byte(uint8_t b, String* out) {
  static const char kHex[] = "0123456789abcdef";
  out->push_back(kHex[b >> 4]);
  out->push_back(kHex[b & 15]);
}

class SystemRandom final : public crypto::IRandom {
 public:
  bool fill(ByteView out) override { return system_random(out); }
};

bool hex_decode(StringView text, ByteView out) {
  if (text.size() != out.size() * 2) return false;
  for (uint64_t i = 0; i < out.size(); ++i) {
    uint8_t v = 0;
    for (uint64_t j = 0; j < 2; ++j) {
      const char c = text[i * 2 + j];
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (d < 0) return false;
      v = static_cast<uint8_t>(v * 16 + d);
    }
    out[i] = v;
  }
  return true;
}

bool random_hex(uint32_t count, bool point, String* out) {
  uint8_t bytes[65];
  if (count > sizeof(bytes) || !system_random(ByteView(bytes, count))) return false;
  if (point) bytes[0] = 4;   // (an uncompressed point, the shape of a P-256 public key)
  for (uint32_t i = 0; i < count; ++i) hex_byte(bytes[i], out);
  return true;
}

}  // namespace

ServiceWorkerHub::Permission* ServiceWorkerHub::find_permission(StringView origin) {
  for (Permission& p : permissions_)
    if (p.origin.view().equals(origin)) return &p;
  return nullptr;
}

const char* ServiceWorkerHub::notification_permission(StringView origin) {
  Lock lock(*this);
  const Permission* p = find_permission(origin);
  return !p ? "default" : p->granted ? "granted" : "denied";
}

void ServiceWorkerHub::apply_permission(StringView origin, bool granted) {
  Permission* p = find_permission(origin);
  if (!p) {
    if (permissions_.count() >= kMaxPermissions) permissions_.remove_at(0);
    Permission fresh(allocator_);
    fresh.origin.assign(origin);
    if (!permissions_.push_back(util::move(fresh))) return;
    p = &permissions_[permissions_.count() - 1];
  }
  p->granted = granted;
}

void ServiceWorkerHub::set_notification_permission(StringView origin, bool granted) {
  Lock lock(*this);
  apply_permission(origin, granted);
  String json(allocator_);
  json.append("{\"t\":\"permission\",\"state\":\"");
  json.append(granted ? "granted" : "denied");
  json.append("\"}");
  emit_to_origin(origin, json.view());
  if (!granted)   // (a blocked origin has nothing on show)
    for (uint64_t i = notifications_.count(); i-- > 0;)
      if (notifications_[i]->origin.view().equals(origin)) remove_notification(i, true);
}

bool ServiceWorkerHub::origin_of(uint32_t client, uint32_t worker, String* origin) {
  if (client != 0) {
    if (const Client* c = find_client(client)) {
      origin->assign(c->origin.view());
      return true;
    }
  }
  if (worker != 0) {
    const Worker* w = find_worker(worker);
    const Registration* r = w ? find_registration(w->registration) : nullptr;
    if (r) {
      origin->assign(r->origin.view());
      return true;
    }
  }
  return false;
}

void ServiceWorkerHub::notify_permission(uint32_t client, uint32_t worker, String* state) {
  Lock lock(*this);
  String origin(allocator_);
  state->assign(origin_of(client, worker, &origin) ? notification_permission(origin.view()) : "denied");
}

void ServiceWorkerHub::notify_request(uint32_t client, uint32_t worker, String* state) {
  Lock lock(*this);
  String origin(allocator_);
  if (!origin_of(client, worker, &origin) || client == 0) {   // (only a document may ask)
    notify_permission(client, worker, state);
    return;
  }
  state->assign(notification_permission(origin.view()));
  if (!state->view().equals("default")) return;
  const int answer = host_ ? host_->notification_prompt(origin.view()) : 0;
  if (answer < 0) {
    state->assign("pending");
    return;
  }
  set_notification_permission(origin.view(), answer > 0);
  state->assign(answer > 0 ? "granted" : "denied");
}

void ServiceWorkerHub::notification_json(const Notification& n, String* json) {
  json->append("{\"id\":");
  json->append_uint(n.id);
  json->append(",\"title\":");
  jq(json, n.title.view());
  json->append(",\"body\":");
  jq(json, n.body.view());
  json->append(",\"tag\":");
  jq(json, n.tag.view());
  json->append(",\"o\":");
  json->append(n.options.view());
  json->push_back('}');
}

void ServiceWorkerHub::remove_notification(uint64_t index, bool tell_host) {
  if (index >= notifications_.count()) return;
  const uint32_t number = notifications_[index]->number;
  notifications_.remove_at(index);
  if (tell_host && number != 0 && host_) host_->notification_close(number);
}

void ServiceWorkerHub::notify_show(uint32_t client, uint32_t reg, StringView title, StringView body, StringView tag, bool sticky, bool renotify, StringView options, StringView action_ids,
                                      StringView action_titles, String* json) {
  Lock lock(*this);
  (void)renotify;   // (a toast cannot be told to stay quiet: a replaced notice is shown again either way)
  String origin(allocator_);
  Registration* r = reg ? find_registration(reg) : nullptr;
  if (reg) {
    if (!r || r->unregistered) {
      error_json(json, "InvalidStateError", "The registration is gone.");
      return;
    }
    origin.assign(r->origin.view());
  } else if (!origin_of(client, 0, &origin)) {
    error_json(json, "NotAllowedError", "The document cannot show notifications.");
    return;
  }
  if (!StringView(notification_permission(origin.view())).equals("granted")) {
    error_json(json, "NotAllowedError", "No notification permission has been granted for this origin.");
    return;
  }
  if (!tag.empty()) {   // a notification with the same tag from the same place is replaced
    for (uint64_t i = 0; i < notifications_.count(); ++i) {
      const Notification& old = *notifications_[i];
      if (old.reg != reg || (reg == 0 && old.client != client) || !old.origin.view().equals(origin.view()) || !old.tag.view().equals(tag)) continue;
      if (reg == 0) {
        String ev(allocator_);
        ev.append("{\"t\":\"notification\",\"id\":");
        ev.append_uint(old.id);
        ev.append(",\"ev\":\"close\"}");
        if (Client* c = find_client(old.client)) emit_to_client(*c, ev.view());
      }
      remove_notification(i, true);
      break;
    }
  }
  if (notifications_.count() >= kMaxNotifications) remove_notification(0, true);
  UniquePointer<Notification> n = UniquePointer<Notification>::make(allocator_, allocator_);
  n->id = next_notification_++;
  if (next_notification_ == 0) next_notification_ = 1;
  n->reg = reg;
  n->client = reg ? 0 : client;
  n->origin.assign(origin.view());
  n->title.assign(title);
  n->body.assign(body);
  n->tag.assign(tag);
  n->options.assign(options.size() >= 2 && options[0] == '{' && options[options.size() - 1] == '}' ? options : StringView("{}"));
  n->actions.assign(action_ids);
  n->number = host_ ? host_->notification_show(origin.view(), title, body, sticky, action_titles) : 0;
  if (n->number == 0) {
    error_json(json, "NotAllowedError", "The notification could not be shown.");
    return;
  }
  const uint32_t id = n->id, number = n->number;
  if (!notifications_.push_back(util::move(n))) {
    host_->notification_close(number);
    error_json(json, "NotAllowedError", "The notification could not be shown.");
    return;
  }
  if (reg == 0) {
    String ev(allocator_);
    ev.append("{\"t\":\"notification\",\"id\":");
    ev.append_uint(id);
    ev.append(",\"ev\":\"show\"}");
    if (Client* c = find_client(client)) emit_to_client(*c, ev.view());
  }
  json->assign("{\"ok\":true,\"id\":");
  json->append_uint(id);
  json->push_back('}');
}

bool ServiceWorkerHub::notify_close(uint32_t client, uint32_t id) {
  Lock lock(*this);
  for (uint64_t i = 0; i < notifications_.count(); ++i) {
    const Notification& n = *notifications_[i];
    if (n.id != id) continue;
    if (n.reg == 0 && n.client != client) return false;   // (a document closes its own; a worker's may be closed from anywhere in its origin)
    if (n.reg == 0) {
      String ev(allocator_);
      ev.append("{\"t\":\"notification\",\"id\":");
      ev.append_uint(n.id);
      ev.append(",\"ev\":\"close\"}");
      if (Client* c = find_client(n.client)) emit_to_client(*c, ev.view());
    }
    remove_notification(i, true);
    return true;
  }
  return false;
}

void ServiceWorkerHub::notify_list(uint32_t reg, StringView tag, String* json) {
  Lock lock(*this);
  json->assign("[");
  bool first = true;
  for (const UniquePointer<Notification>& n : notifications_) {
    if (n->reg != reg || reg == 0 || (!tag.empty() && !n->tag.view().equals(tag))) continue;
    if (!first) json->push_back(',');
    first = false;
    notification_json(*n, json);
  }
  json->push_back(']');
}

// The notice was clicked: a document's notification gets click and close; a registration's worker gets notificationclick (and may open a window).
void ServiceWorkerHub::notification_activated(uint32_t number, int32_t action) {
  Lock lock(*this);
  for (uint64_t i = 0; i < notifications_.count(); ++i) {
    if (notifications_[i]->number != number) continue;
    String info(allocator_), arg(allocator_);
    notification_json(*notifications_[i], &info);
    const uint32_t reg = notifications_[i]->reg, client = notifications_[i]->client, id = notifications_[i]->id;
    String action_id(allocator_);
    if (action >= 0) {   // (the name of the button: line `action` of the list)
      StringView rest = notifications_[i]->actions.view(), line;
      for (int32_t k = 0; rest.next_token('\n', &line); ++k)
        if (k == action) {
          action_id.assign(line);
          break;
        }
    }
    remove_notification(i, false);
    if (reg == 0) {
      Client* c = find_client(client);
      if (!c) return;
      for (const char* ev : {"click", "close"}) {
        String text(allocator_);
        text.append("{\"t\":\"notification\",\"id\":");
        text.append_uint(id);
        text.append(",\"ev\":\"");
        text.append(ev);
        text.append("\"}");
        emit_to_client(*c, text.view());
      }
      return;
    }
    Registration* r = find_registration(reg);
    Worker* w = r && r->active ? find_worker(r->active) : nullptr;
    if (!w || w->state != Worker::State::Activated) return;
    arg.append("{\"notification\":");
    arg.append(info.view());
    arg.append(",\"action\":");
    jq(&arg, action_id.view());
    arg.push_back('}');
    dispatch_extendable(*w, "notificationclick", arg.view());
    return;
  }
}

void ServiceWorkerHub::notification_closed(uint32_t number) {
  Lock lock(*this);
  for (uint64_t i = 0; i < notifications_.count(); ++i) {
    if (notifications_[i]->number != number) continue;
    String info(allocator_), arg(allocator_);
    notification_json(*notifications_[i], &info);
    const uint32_t reg = notifications_[i]->reg, client = notifications_[i]->client, id = notifications_[i]->id;
    remove_notification(i, false);
    if (reg == 0) {
      if (Client* c = find_client(client)) {
        String text(allocator_);
        text.append("{\"t\":\"notification\",\"id\":");
        text.append_uint(id);
        text.append(",\"ev\":\"close\"}");
        emit_to_client(*c, text.view());
      }
      return;
    }
    Registration* r = find_registration(reg);
    Worker* w = r && r->active ? find_worker(r->active) : nullptr;
    if (!w || w->state != Worker::State::Activated) return;
    arg.append("{\"notification\":");
    arg.append(info.view());
    arg.append(",\"action\":\"\"}");
    dispatch_extendable(*w, "notificationclose", arg.view());
    return;
  }
}

void ServiceWorkerHub::push_json(const Registration& r, String* json) {
  json->append("{\"endpoint\":");
  jq(json, r.push_endpoint.view());
  json->append(",\"p256dh\":\"");
  json->append(r.push_p256dh.view());
  json->append("\",\"auth\":\"");
  json->append(r.push_auth.view());
  json->append("\",\"key\":\"");
  json->append(r.push_key.view());
  json->append("\"}");
}

void ServiceWorkerHub::push_subscribe(uint32_t reg, bool user_visible_only, StringView key_hex, String* json) {
  Lock lock(*this);
  Registration* r = find_registration(reg);
  if (!r || r->unregistered || r->active == 0) {
    error_json(json, "InvalidStateError", "Subscription failed - no active Service Worker");
    return;
  }
  if (!user_visible_only) {
    error_json(json, "NotAllowedError", "Subscription failed - userVisibleOnly is false");
    return;
  }
  if (!StringView(notification_permission(r->origin.view())).equals("granted")) {
    error_json(json, "NotAllowedError", "Registration failed - permission denied");
    return;
  }
  for (const char c : key_hex)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      error_json(json, "InvalidAccessError", "Subscription failed - the applicationServerKey is not valid");
      return;
    }
  if (key_hex.size() > 256) {
    error_json(json, "InvalidAccessError", "Subscription failed - the applicationServerKey is not valid");
    return;
  }
  if (!r->push_endpoint.empty()) {
    if (!key_hex.empty() && !r->push_key.empty() && !r->push_key.view().equals(key_hex)) {
      error_json(json, "InvalidStateError", "Subscription failed - a subscription with a different applicationServerKey already exists");
      return;
    }
  } else {
    String endpoint(allocator_), p256dh(allocator_), auth(allocator_), token(allocator_), secret(allocator_);
    uint8_t priv[32], point[crypto::kMaxEcPoint];
    SystemRandom random;
    if (!random_hex(24, false, &token) || !random_hex(16, false, &auth) || !crypto::ec_generate_private(crypto::EcCurve::P256, random, priv) ||
        !crypto::ec_public_key(crypto::EcCurve::P256, priv, point)) {
      error_json(json, "AbortError", "Subscription failed - no source of random numbers");
      return;
    }
    for (uint32_t i = 0; i < 65; ++i) hex_byte(point[i], &p256dh);
    for (uint32_t i = 0; i < 32; ++i) hex_byte(priv[i], &secret);
    endpoint.assign("https://");
    endpoint.append(kPushHost);
    endpoint.append("/wpush/v2/");
    endpoint.append(token.view());
    r->push_endpoint = util::move(endpoint);
    r->push_private = util::move(secret);
    r->push_p256dh = util::move(p256dh);
    r->push_auth = util::move(auth);
    r->push_key.assign(key_hex);
  }
  json->assign("{\"ok\":true,\"sub\":");
  push_json(*r, json);
  json->push_back('}');
}

void ServiceWorkerHub::push_get(uint32_t reg, String* json) {
  Lock lock(*this);
  const Registration* r = find_registration(reg);
  json->clear();
  if (r && !r->unregistered && !r->push_endpoint.empty()) push_json(*r, json);
}

bool ServiceWorkerHub::push_unsubscribe(uint32_t reg) {
  Lock lock(*this);
  Registration* r = find_registration(reg);
  if (!r || r->push_endpoint.empty()) return false;
  push_retire(*r);
  return true;
}

// The subscription is given up: its endpoint answers 410 from now on (for the last few).
void ServiceWorkerHub::push_retire(Registration& r) {
  const StringView endpoint = r.push_endpoint.view();
  const uint64_t slash = endpoint.size() ? endpoint.find("/wpush/v2/") : StringView::npos;
  if (slash != StringView::npos) {
    if (push_gone_.count() >= 64) push_gone_.remove_at(0);
    String token(allocator_);
    token.assign(endpoint.substr(slash + 10));
    push_gone_.push_back(util::move(token));
  }
  r.push_endpoint.clear();
  r.push_p256dh.clear();
  r.push_auth.clear();
  r.push_key.clear();
  r.push_private.clear();
}

void ServiceWorkerHub::push_state(uint32_t reg, String* state) {
  Lock lock(*this);
  const Registration* r = find_registration(reg);
  const char* p = r ? notification_permission(r->origin.view()) : "denied";
  state->assign(StringView(p).equals("default") ? "prompt" : p);
}

bool ServiceWorkerHub::push_deliver(StringView endpoint, StringView data, bool has_data) {
  Lock lock(*this);
  if (endpoint.empty()) return false;
  for (const UniquePointer<Registration>& r : registrations_) {
    if (r->unregistered || !r->push_endpoint.view().equals(endpoint)) continue;
    Worker* w = r->active ? find_worker(r->active) : nullptr;
    if (!w || w->state != Worker::State::Activated) return false;
    String arg(allocator_);
    arg.append("{");
    if (has_data) {
      arg.append("\"data\":\"");
      for (const char c : data) hex_byte(static_cast<uint8_t>(c), &arg);
      arg.append("\"");
    }
    arg.append("}");
    return dispatch_extendable(*w, "push", arg.view());
  }
  return false;
}

bool ServiceWorkerHub::push_expire(StringView endpoint) {
  Lock lock(*this);
  if (endpoint.empty()) return false;
  for (const UniquePointer<Registration>& r : registrations_) {
    if (r->unregistered || !r->push_endpoint.view().equals(endpoint)) continue;
    String arg(allocator_);
    arg.append("{\"old\":");
    push_json(*r, &arg);
    arg.push_back('}');
    push_retire(*r);
    Worker* w = r->active ? find_worker(r->active) : nullptr;
    return w && w->state == Worker::State::Activated && dispatch_extendable(*w, "pushsubscriptionchange", arg.view());
  }
  return false;
}

bool ServiceWorkerHub::push_handles(StringView url) {
  WebUrl parsed(allocator_);
  return WebUrl::parse(allocator_, url, nullptr, &parsed) && parsed.scheme.equals("https") && parsed.host.view().equals(kPushHost);
}

namespace {

void push_answer(NetResponse* response, uint32_t status, const char* reason, StringView url, StringView extra) {
  response->status = status;
  response->reason.assign(reason);
  response->url.assign(url);
  response->headers.assign("Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: Location, TTL\r\n");
  response->headers.append(extra);
  response->body.clear();
}

}  // namespace

// The push service of RFC 8030. Every outcome is an HTTP answer, never a failure of the request.
void ServiceWorkerHub::push_request(const FetchOptions& options, NetResponse* response) {
  Lock lock(*this);
  WebUrl url(allocator_);
  if (!WebUrl::parse(allocator_, options.url, nullptr, &url)) {
    push_answer(response, 400, "Bad Request", options.url, "");
    return;
  }
  const StringView path = url.path.view(), prefix = "/wpush/v2/";
  if (!path.starts_with(prefix) || path.size() == prefix.size() || path.substr(prefix.size()).contains('/')) {
    push_answer(response, 404, "Not Found", options.url, "");
    return;
  }
  if (StringView(options.method).equals("OPTIONS")) {   // (the preflight of a page acting as an application server)
    push_answer(response, 204, "No Content", options.url,
                "Access-Control-Allow-Methods: POST\r\nAccess-Control-Allow-Headers: content-encoding, content-type, ttl, urgency, topic, authorization, crypto-key\r\n");
    return;
  }
  if (!StringView(options.method).equals("POST")) {
    push_answer(response, 405, "Method Not Allowed", options.url, "Allow: POST\r\n");
    return;
  }
  const StringView token = path.substr(prefix.size());
  Registration* r = nullptr;
  for (const UniquePointer<Registration>& candidate : registrations_) {
    const StringView endpoint = candidate->push_endpoint.view();
    if (!candidate->unregistered && !endpoint.empty() && endpoint.ends_with(token) && endpoint.size() > token.size() && endpoint[endpoint.size() - token.size() - 1] == '/') r = candidate.get();
  }
  if (!r) {
    bool gone = false;
    for (const String& g : push_gone_) gone = gone || g.view().equals(token);
    push_answer(response, gone ? 410 : 404, gone ? "Gone" : "Not Found", options.url, "");
    return;
  }
  StringView value;
  uint64_t ttl = 0;
  if (!header_value(options.headers, "ttl", &value) || !value.parse_uint(&ttl)) {   // (RFC 8030: a push without TTL is refused)
    push_answer(response, 400, "Bad Request", options.url, "");
    return;
  }
  if (header_value(options.headers, "urgency", &value) && !(value.equals("very-low") || value.equals("low") || value.equals("normal") || value.equals("high"))) {
    push_answer(response, 400, "Bad Request", options.url, "");
    return;
  }
  if (header_value(options.headers, "topic", &value)) {
    bool good = !value.empty() && value.size() <= 32;
    for (const char c : value) good = good && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
    if (!good) {
      push_answer(response, 400, "Bad Request", options.url, "");
      return;
    }
  }
  if (options.body.size() > kWebPushMaxBody) {
    push_answer(response, 413, "Payload Too Large", options.url, "");
    return;
  }
  if (!r->push_key.empty()) {   // the subscription was made for one sender: only it may push
    uint8_t key[65];
    StringView authorization, crypto_key;
    header_value(options.headers, "authorization", &authorization);
    header_value(options.headers, "crypto-key", &crypto_key);
    String audience(allocator_);
    audience.assign("https://");
    audience.append(kPushHost);
    const Vapid vapid = hex_decode(r->push_key.view(), ByteView(key, 65))
                            ? vapid_check(allocator_, authorization, crypto_key, ConstByteView(key, 65), audience.view(), host_ ? host_->now_ms() / 1000 : 0)
                            : Vapid::Invalid;
    if (vapid != Vapid::Ok) {
      push_answer(response, vapid == Vapid::Missing ? 401 : 403, vapid == Vapid::Missing ? "Unauthorized" : "Forbidden", options.url, "");
      return;
    }
  }
  ByteBuffer plain(allocator_);
  const bool has_data = options.body.size() != 0;
  if (has_data) {
    StringView coding;
    uint8_t priv[32], pub[65], auth[16];
    if (!header_value(options.headers, "content-encoding", &coding) || !coding.equals("aes128gcm") || !hex_decode(r->push_private.view(), ByteView(priv, 32)) ||
        !hex_decode(r->push_p256dh.view(), ByteView(pub, 65)) || !hex_decode(r->push_auth.view(), ByteView(auth, 16)) ||
        !webpush_decrypt(priv, ConstByteView(pub, 65), ConstByteView(auth, 16), options.body, &plain)) {
      push_answer(response, 400, "Bad Request", options.url, "");
      return;
    }
  }
  Worker* w = r->active ? find_worker(r->active) : nullptr;
  if (!w || w->state != Worker::State::Activated) {   // (no worker to take it: the subscription is as good as gone)
    push_answer(response, 503, "Service Unavailable", options.url, "Retry-After: 60\r\n");
    return;
  }
  String arg(allocator_), location(allocator_), extra(allocator_);
  arg.append("{");
  if (has_data) {
    arg.append("\"data\":\"");
    for (const uint8_t b : plain.view()) hex_byte(b, &arg);
    arg.append("\"");
  }
  arg.append("}");
  dispatch_extendable(*w, "push", arg.view());   // (a handler that fails does not un-deliver the message)
  location.assign(r->push_endpoint.view());
  location.append("/m/");
  location.append_uint(++push_messages_);
  extra.append("Location: ");
  extra.append(location.view());
  extra.append("\r\nTTL: ");
  extra.append_uint(ttl);
  extra.append("\r\n");
  push_answer(response, 201, "Created", options.url, extra.view());
}

// --- The lifecycle ---------------------------------------------------------------------------------------------------------------

// A waiting worker becomes the active one: the old one is replaced, `activate` is dispatched.
void ServiceWorkerHub::activate_worker(Registration& r, Worker& w) {
  const uint32_t old = r.active;
  r.waiting = 0;
  r.active = w.id;
  w.state = Worker::State::Activating;
  if (old != 0)
    if (Worker* o = find_worker(old)) {
      o->state = Worker::State::Redundant;
      stop_worker(*o);
      emit_state(r, *o);
    }
  emit_state(r, w);
  if (old != 0) {   // the documents the old worker was in charge of have a new controller
    String event(allocator_);
    event.append("{\"t\":\"controllerchange\",\"worker\":");
    event.append_uint(w.id);
    event.append(",\"reg\":");
    registration_json(r, &event);
    event.push_back('}');
    for (uint64_t i = 0; i < clients_.count(); ++i)
      if (clients_[i]->controller == r.id) emit_to_client(*clients_[i], event.view());
  }
  String message(allocator_);
  if (dispatch_lifecycle(w, "activate", &message)) {
    w.state = Worker::State::Activated;
    emit_state(r, w);
  } else {
    w.state = Worker::State::Redundant;
    r.active = 0;
    stop_worker(w);
    emit_state(r, w);
  }
}

void ServiceWorkerHub::claim_clients(Registration& r) {
  for (uint64_t i = 0; i < clients_.count(); ++i) {
    Client& c = *clients_[i];
    if (!c.origin.view().equals(r.origin.view()) || !c.url.view().starts_with(r.scope.view()) || c.controller == r.id) continue;
    // the registration with the longest scope that has an active worker is in charge, as for any navigation
    Registration* best = match_scope(c.url.view());
    if (best && best->id != r.id) continue;
    c.controller = r.id;
    String event(allocator_);
    event.append("{\"t\":\"controllerchange\",\"worker\":");
    event.append_uint(r.active);
    event.append(",\"reg\":");
    registration_json(r, &event);
    event.push_back('}');
    emit_to_client(c, event.view());
  }
  touch();
}

void ServiceWorkerHub::worker_skip_waiting(uint32_t worker) {
  if (Worker* w = find_worker(worker)) w->skip_waiting = true;
}

void ServiceWorkerHub::worker_claim(uint32_t worker) {
  Worker* w = find_worker(worker);
  Registration* r = w ? find_registration(w->registration) : nullptr;
  if (r && r->active == worker) claim_clients(*r);
}

void ServiceWorkerHub::fire_waiting_syncs(Registration& r) {
  Worker* w = r.active ? find_worker(r.active) : nullptr;
  if (!w || w->state != Worker::State::Activated || r.sync_tags.count() == 0) return;
  String tag(allocator_), arg(allocator_);
  tag.assign(r.sync_tags[0].view());
  r.sync_tags.remove_at(0);
  arg.append("{\"tag\":");
  jq(&arg, tag.view());
  arg.append(",\"lastChance\":false}");
  dispatch_extendable(*w, "sync", arg.view());
}

bool ServiceWorkerHub::busy() {
  Lock lock(*this, true);
  if (!lock.held() || !host_) return false;
  for (uint64_t i = 0; i < registrations_.count(); ++i) {
    const Registration& r = *registrations_[i];
    if (r.installing != 0) {
      Worker* w = find_worker(r.installing);
      if (w && !w->install_started) return true;
    }
    if (r.update_due && !r.unregistered) return true;
    if (r.sync_tags.count() > 0 && r.active != 0) return true;
    if (r.waiting != 0) {
      Worker* w = find_worker(r.waiting);
      if (!w) continue;
      bool controlled = false;
      for (uint64_t k = 0; k < clients_.count() && !controlled; ++k) controlled = r.active != 0 && clients_[k]->controller == r.id;
      if (r.active == 0 || w->skip_waiting || !controlled) return true;
    }
    if (r.unregistered) {
      bool controlled = false;
      for (uint64_t k = 0; k < clients_.count() && !controlled; ++k) controlled = clients_[k]->controller == r.id;
      if (!controlled) return true;
    }
  }
  return false;
}

// One step for each registration: installing, activating, a sync, the clean up of an unregistered one; idle workers are stopped.
void ServiceWorkerHub::pump(uint64_t now_ms) {
  Lock lock(*this);
  if (!host_) return;
  now_ms_ = now_ms;
  for (uint64_t i = 0; i < registrations_.count(); ++i) {
    Registration& r = *registrations_[i];
    if (r.installing != 0) {
      Worker* w = find_worker(r.installing);
      if (w && !w->install_started) {
        w->install_started = true;
        String message(allocator_);
        if (dispatch_lifecycle(*w, "install", &message)) {
          r.installing = 0;
          if (r.waiting != 0)
            if (Worker* old = find_worker(r.waiting)) make_redundant(*old, true);
          r.waiting = w->id;
          w->state = Worker::State::Installed;
          emit_state(r, *w);
        } else {
          make_redundant(*w, true);
          if (r.installing == 0 && r.waiting == 0 && r.active == 0) r.unregistered = true;
        }
        continue;
      }
    }
    if (r.waiting != 0 && !r.unregistered) {
      Worker* w = find_worker(r.waiting);
      bool controlled = false;
      for (uint64_t k = 0; k < clients_.count() && !controlled; ++k) controlled = r.active != 0 && clients_[k]->controller == r.id;
      if (w && (r.active == 0 || w->skip_waiting || !controlled)) {
        activate_worker(r, *w);
        continue;
      }
    }
    if (r.update_due && !r.unregistered) {
      String message(allocator_), name(allocator_);
      start_update(r, &message, &name, false);
      continue;
    }
    if (r.sync_tags.count() > 0 && r.active != 0) {
      fire_waiting_syncs(r);
      continue;
    }
  }
  // unregistered registrations nobody is controlled by any more go; redundant workers without a registration go
  for (uint64_t i = 0; i < registrations_.count();) {
    Registration& r = *registrations_[i];
    bool controlled = false;
    for (uint64_t k = 0; k < clients_.count() && !controlled; ++k) controlled = clients_[k]->controller == r.id;
    if (r.unregistered && !controlled) {
      remove_registration(r);
      continue;
    }
    ++i;
  }
  for (uint64_t i = 0; i < workers_.count(); ++i) {
    Worker& w = *workers_[i];
    if (!w.runtime) continue;
    if (now_ms - w.last_used_ms > kIdleStopMs) {
      stop_worker(w);
      continue;
    }
    Running running(w);
    w.runtime->page->run_due_timers(static_cast<double>(now_ms));
  }
  for (uint64_t i = 0; i < workers_.count();) {   // (the records of redundant workers are kept a while: a page may still hold its object)
    if (workers_.count() > kMaxWorkers / 2 && workers_[i]->state == Worker::State::Redundant && !find_registration(workers_[i]->registration)) workers_.remove_at(i);
    else ++i;
  }
}

// --- Fetch ---------------------------------------------------------------------------------------------------------------------

static const char* mode_name(FetchOptions::Mode m) {
  switch (m) {
    case FetchOptions::Mode::Cors: return "cors";
    case FetchOptions::Mode::NoCors: return "no-cors";
    case FetchOptions::Mode::SameOrigin: return "same-origin";
    case FetchOptions::Mode::Navigate: return "navigate";
    case FetchOptions::Mode::Subresource: return "no-cors";
  }
  return "cors";
}

bool ServiceWorkerHub::controls(StringView url) {
  Lock lock(*this);
  return match_scope(url) != nullptr;
}

ServiceWorkerHub::Intercept ServiceWorkerHub::intercept(uint32_t client, const FetchOptions& options, StringView destination, bool navigation, NetResponse* response) {
  Lock lock(*this);
  if (!host_ || !(options.url.starts_with("http://") || options.url.starts_with("https://"))) return Intercept::Pass;
  Client* c = client ? find_client(client) : nullptr;
  Registration* r = nullptr;
  if (navigation && !client) r = match_scope(options.url);
  else if (c && c->controller) r = find_registration(c->controller);
  if (!r) return Intercept::Pass;
  Worker* w = r->active ? find_worker(r->active) : nullptr;
  if (!w || w->state != Worker::State::Activated) return Intercept::Pass;
  if (!ensure_running(*w)) return Intercept::Pass;
  Running running(*w);

  const bool is_navigation = navigation || options.mode == FetchOptions::Mode::Navigate;
  String info(allocator_), id(allocator_);
  info.append("{\"method\":");
  jq(&info, options.method);
  info.append(",\"url\":");
  jq(&info, options.url);
  info.append(",\"headers\":");
  jq(&info, options.headers);
  info.append(",\"mode\":");
  jq(&info, is_navigation ? "navigate" : mode_name(options.mode));
  info.append(",\"credentials\":");
  jq(&info, options.credentials == FetchOptions::Credentials::Omit ? "omit" : options.credentials == FetchOptions::Credentials::Include ? "include" : "same-origin");
  info.append(",\"redirect\":");
  jq(&info, options.redirect == FetchOptions::Redirect::Error ? "error" : options.redirect == FetchOptions::Redirect::Manual ? "manual" : "follow");
  info.append(",\"referrer\":");
  jq(&info, options.referrer_given ? options.referrer : (c ? c->url.view() : StringView("")));
  info.append(",\"destination\":");
  jq(&info, is_navigation && destination.empty() ? StringView("document") : destination);
  info.append(",\"navigation\":");
  info.append(is_navigation ? "true" : "false");
  info.append(",\"client\":");
  if (c) id.append_uint(c->id);
  jq(&info, id.view());
  info.append(",\"preload\":");
  info.append(r->preload_enabled && is_navigation && options.method.equals("GET") ? "true" : "false");
  info.append(",\"preloadHeader\":");
  jq(&info, r->preload_header.empty() ? StringView("true") : r->preload_header.view());
  info.push_back('}');

  w->token = next_token_++;
  w->done = false;
  w->ok = false;
  w->answered = false;
  w->answer = FetchAnswer(allocator_);
  using Arg = DomBindings::HookArg;
  String body_text(allocator_);
  body_text.assign(StringView(reinterpret_cast<const char*>(options.body.data()), options.body.size()));
  const Arg args[3] = {Arg::of(static_cast<double>(w->token)), Arg::of(info.view()), Arg::of(body_text.view())};
  const uint32_t token = w->token;
  if (!w->runtime->page->call_global("__sw_dispatch_fetch", args, 3)) return Intercept::Pass;
  const bool settled = run_to_completion(*w, token);
  w->last_used_ms = host_->now_ms();
  if (!settled || !w->answered || w->answer.kind == 0) {
    if (w->runtime) drain(*w);
    return Intercept::Pass;
  }
  FetchAnswer answer = util::move(w->answer);
  if (w->runtime) drain(*w);   // (waitUntil work: a cache updated after the answer went out)
  if (answer.kind == 2) {
    response->error.assign("Failed to fetch");
    return Intercept::Failed;
  }
  // a redirect the worker answered with is followed (the next request is for the worker again if its scope has it)
  const bool redirect_status = answer.status == 301 || answer.status == 302 || answer.status == 303 || answer.status == 307 || answer.status == 308;
  StringView location;
  if (redirect_status && header_value(answer.headers.view(), "location", &location) && options.redirect == FetchOptions::Redirect::Follow && !answer.type.view().equals("opaqueredirect")) {
    WebUrl base(allocator_), next(allocator_);
    String next_text(allocator_);
    if (!parse_url(allocator_, options.url, &base) || !WebUrl::parse(allocator_, location, &base, &next)) {
      response->error.assign("Failed to fetch");
      return Intercept::Failed;
    }
    next.serialize(&next_text, true);
    FetchOptions follow = options;
    follow.url = next_text.view();
    const bool to_get = answer.status == 303 || ((answer.status == 301 || answer.status == 302) && options.method.equals("POST"));
    if (to_get) {
      follow.method = "GET";
      follow.body = ConstByteView();
    }
    if (follow.max_redirects == 0) {
      response->error.assign("Failed to fetch");
      return Intercept::Failed;
    }
    --follow.max_redirects;
    const Intercept again = intercept(client, follow, destination, navigation, response);
    if (again != Intercept::Pass) {
      if (again == Intercept::Responded) response->redirected = true;
      return again;
    }
    if (!host_->http_request(follow, response)) return Intercept::Failed;
    response->redirected = true;
    return Intercept::Responded;
  }
  response->status = answer.type.view().equals("opaque") ? 0 : answer.status;
  response->opaque = answer.type.view().equals("opaque");
  response->reason.assign(answer.status_text.view());
  response->url.assign(answer.url.empty() ? options.url : answer.url.view());
  response->headers.assign(answer.headers.view());
  response->body.clear();
  response->body.append(answer.body.view().bytes());
  response->redirected = answer.redirected;
  return Intercept::Responded;
}

// --- Clients, from the worker's side ---------------------------------------------------------------------------------------------

void ServiceWorkerHub::worker_clients(uint32_t worker, bool uncontrolled, StringView type, String* json) {
  Worker* w = find_worker(worker);
  Registration* r = w ? find_registration(w->registration) : nullptr;
  json->assign("[");
  bool first = true;
  const bool windows = type.empty() || type.equals("window") || type.equals("all");
  for (uint64_t i = 0; r && windows && i < clients_.count(); ++i) {
    const Client& c = *clients_[i];
    if (!c.origin.view().equals(r->origin.view())) continue;
    if (c.controller != r->id && !uncontrolled) continue;
    if (!first) json->push_back(',');
    first = false;
    json->append("{\"id\":\"");
    json->append_uint(c.id);
    json->append("\",\"url\":");
    jq(json, c.url.view());
    json->append(",\"type\":\"window\",\"frameType\":\"top-level\",\"visibilityState\":\"visible\",\"focused\":false,\"controlled\":");
    json->append(c.controller == r->id ? "true" : "false");
    json->push_back('}');
  }
  json->push_back(']');
}

bool ServiceWorkerHub::worker_client(uint32_t worker, StringView id, String* json) {
  Worker* w = find_worker(worker);
  Registration* r = w ? find_registration(w->registration) : nullptr;
  json->clear();
  for (uint64_t i = 0; r && i < clients_.count(); ++i) {
    const Client& c = *clients_[i];
    String cid(allocator_);
    cid.append_uint(c.id);
    if (!cid.view().equals(id) || !c.origin.view().equals(r->origin.view())) continue;
    json->append("{\"id\":\"");
    json->append_uint(c.id);
    json->append("\",\"url\":");
    jq(json, c.url.view());
    json->append(",\"type\":\"window\",\"frameType\":\"top-level\",\"visibilityState\":\"visible\",\"focused\":false,\"controlled\":");
    json->append(c.controller == r->id ? "true" : "false");
    json->push_back('}');
    return true;
  }
  return false;
}

bool ServiceWorkerHub::worker_client_post(uint32_t worker, StringView id, StringView wire, StringView ports) {
  Worker* w = find_worker(worker);
  Registration* r = w ? find_registration(w->registration) : nullptr;
  for (uint64_t i = 0; r && i < clients_.count(); ++i) {
    Client& c = *clients_[i];
    String cid(allocator_);
    cid.append_uint(c.id);
    if (!cid.view().equals(id) || !c.origin.view().equals(r->origin.view())) continue;
    route_ports(ports, false, c.id, true, worker);
    String event(allocator_);
    event.append("{\"t\":\"message\",\"data\":");
    jq(&event, wire);
    event.append(",\"ports\":");
    jq(&event, ports);
    event.append(",\"worker\":");
    event.append_uint(worker);
    event.append(",\"origin\":");
    jq(&event, r->origin.view());
    event.push_back('}');
    emit_to_client(c, event.view());
    return true;
  }
  return false;
}

bool ServiceWorkerHub::worker_client_navigate(uint32_t worker, StringView id, StringView url, String* json) {
  Worker* w = find_worker(worker);
  Registration* r = w ? find_registration(w->registration) : nullptr;
  json->clear();
  if (!r) return false;
  WebUrl base(allocator_), target(allocator_);
  parse_url(allocator_, r->scope.view(), &base);
  if (!WebUrl::parse(allocator_, url, &base, &target)) return false;
  String origin(allocator_), text(allocator_);
  target.origin(&origin);
  target.serialize(&text, true);
  if (!origin.view().equals(r->origin.view())) return false;
  for (uint64_t i = 0; i < clients_.count(); ++i) {
    Client& c = *clients_[i];
    String cid(allocator_);
    cid.append_uint(c.id);
    if (!cid.view().equals(id) || !c.origin.view().equals(r->origin.view()) || c.controller != r->id) continue;
    String event(allocator_);
    event.append("{\"t\":\"navigate\",\"url\":");
    jq(&event, text.view());
    event.push_back('}');
    emit_to_client(c, event.view());
    json->append("{\"id\":\"");
    json->append_uint(c.id);
    json->append("\",\"url\":");
    jq(json, text.view());
    json->append(",\"type\":\"window\",\"frameType\":\"top-level\",\"visibilityState\":\"visible\",\"focused\":false}");
    return true;
  }
  return false;
}

bool ServiceWorkerHub::worker_open_window(uint32_t worker, StringView url) {
  Worker* w = find_worker(worker);
  Registration* r = w ? find_registration(w->registration) : nullptr;
  if (!r || !host_) return false;
  WebUrl base(allocator_), target(allocator_);
  parse_url(allocator_, r->scope.view(), &base);
  if (!WebUrl::parse(allocator_, url, &base, &target)) return false;
  String text(allocator_);
  target.serialize(&text, true);
  return host_->open_window(text.view(), StringView(), false);
}

void ServiceWorkerHub::worker_registration(uint32_t worker, String* json) {
  Worker* w = find_worker(worker);
  Registration* r = w ? find_registration(w->registration) : nullptr;
  json->clear();
  if (r) registration_json(*r, json);
}

void ServiceWorkerHub::worker_update(uint32_t worker, String* json) {
  Worker* w = find_worker(worker);
  Registration* r = w ? find_registration(w->registration) : nullptr;
  if (!r) {
    error_json(json, "InvalidStateError", "The registration is gone.");
    return;
  }
  api_update(r->id, json);
}

bool ServiceWorkerHub::worker_unregister(uint32_t worker) {
  Worker* w = find_worker(worker);
  return w && api_unregister(w->registration);
}

bool ServiceWorkerHub::worker_sync_register(uint32_t worker, StringView tag) {
  Worker* w = find_worker(worker);
  return w && api_sync_register(w->registration, tag);
}

void ServiceWorkerHub::worker_sync_tags(uint32_t worker, String* json) {
  Worker* w = find_worker(worker);
  if (w) api_sync_tags(w->registration, json);
  else json->assign("[]");
}

void ServiceWorkerHub::worker_preload(uint32_t worker, StringView op, StringView value, String* json) {
  Worker* w = find_worker(worker);
  if (w) api_preload(w->registration, op, value, json);
  else json->clear();
}

// --- Saving --------------------------------------------------------------------------------------------------------------------

uint64_t ServiceWorkerHub::version() {
  Lock lock(*this);
  return version_;
}

void ServiceWorkerHub::snapshot(List<SavedRegistration>* out) {
  Lock lock(*this);
  for (uint64_t i = 0; i < registrations_.count(); ++i) {
    const Registration& r = *registrations_[i];
    if (r.unregistered) continue;
    const uint32_t keep = r.active ? r.active : r.waiting;
    Worker* w = keep ? find_worker(keep) : nullptr;
    if (!w || w->state == Worker::State::Redundant) continue;
    SavedRegistration s(allocator_);
    s.origin.assign(r.origin.view());
    s.scope.assign(r.scope.view());
    s.script_url.assign(r.script_url.view());
    s.update_via_cache.assign(r.update_via_cache.view());
    s.type.assign(r.type.view());
    s.source.assign(w->source.view());
    for (const Import& im : w->imports) {
      s.imports.append(im.url.view());
      s.imports.push_back('\n');
      s.imports.append_uint(im.source.size());
      s.imports.push_back('\n');
      s.imports.append(im.source.view());
    }
    s.preload_enabled = r.preload_enabled;
    s.preload_header.assign(r.preload_header.view());
    out->push_back(util::move(s));
  }
}

bool ServiceWorkerHub::restore(SavedRegistration&& saved) {
  Lock lock(*this);
  if (registrations_.count() >= kMaxRegistrations) return false;
  auto made = UniquePointer<Registration>::make(allocator_, allocator_);
  if (!made) return false;
  made->id = next_registration_++;
  made->origin = util::move(saved.origin);
  made->scope = util::move(saved.scope);
  made->script_url = util::move(saved.script_url);
  made->update_via_cache = util::move(saved.update_via_cache);
  made->type = util::move(saved.type);
  made->preload_enabled = saved.preload_enabled;
  made->preload_header = util::move(saved.preload_header);
  made->last_update_ms = host_ ? host_->now_ms() : 0;
  Registration* r = made.get();
  registrations_.push_back(util::move(made));
  List<Import> imports(allocator_);
  for (StringView rest = saved.imports.view(); !rest.empty();) {   // (url, length and bytes of each import)
    const uint64_t nl = rest.find('\n');
    if (nl == StringView::npos) break;
    Import im(allocator_);
    im.url.assign(rest.first(nl));
    rest = rest.substr(nl + 1);
    const uint64_t nl2 = rest.find('\n');
    if (nl2 == StringView::npos) break;
    uint64_t length = 0;
    bool valid = nl2 > 0 && nl2 <= 12;
    for (const char ch : rest.first(nl2)) {
      if (ch < '0' || ch > '9') valid = false;
      else length = length * 10 + static_cast<uint64_t>(ch - '0');
    }
    rest = rest.substr(nl2 + 1);
    if (!valid || length > rest.size()) break;
    im.source.assign(rest.first(length));
    rest = rest.substr(length);
    imports.push_back(util::move(im));
  }
  Worker* w = nullptr;
  if (!new_worker(*r, util::move(saved.source), &imports, &w)) return false;
  w->state = Worker::State::Activated;
  w->install_started = true;
  r->active = w->id;
  return true;
}

void ServiceWorkerHub::clear_origin(StringView origin) {
  Lock lock(*this);
  for (uint64_t i = 0; i < registrations_.count();) {
    if (registrations_[i]->origin.view().equals(origin)) remove_registration(*registrations_[i]);
    else ++i;
  }
}

void ServiceWorkerHub::clear_all() {
  Lock lock(*this);
  while (registrations_.count() > 0) remove_registration(*registrations_[0]);
}

}  // namespace browser
