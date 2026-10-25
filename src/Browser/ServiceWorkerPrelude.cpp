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

// The script side of the Cache API (caches, Cache, CacheStorage) and of service workers: navigator.serviceWorker and the objects it gives (ServiceWorker,
// ServiceWorkerRegistration), and what a worker's global scope has (the lifecycle and fetch events, clients, skipWaiting, importScripts). The natives are in
// DomBindingsServiceWorker.cpp; the hub they reach is ServiceWorker.hpp.
#include "Browser/ServiceWorkerPrelude.hpp"

namespace browser {

namespace {

const char kCachePrelude[] = R"JSPRELUDE((function (g) {
  if (g.isSecureContext === false || typeof __cache_names !== 'function') return;
  var hide = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true }); };
  var method = function (proto, name, fn) { Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true }); };
  var tag = function (ctor, name) { try { Object.defineProperty(ctor.prototype, Symbol.toStringTag, { value: name, configurable: true }); } catch (e) { } };
  var iface = function (name) {
    var f = function () { throw new TypeError('Illegal constructor'); };
    Object.defineProperty(f, 'name', { value: name });
    Object.defineProperty(f.prototype, 'constructor', { value: f, writable: true, configurable: true });
    tag(f, name);
    hide(g, name, f);
    return f;
  };
  var Cache = iface('Cache'), CacheStorage = iface('CacheStorage');
  var fail = function (on, what, message) { return new TypeError("Failed to execute '" + what + "' on '" + on + "': " + message); };
  var needArgs = function (on, what, n, count) { if (count < n) throw fail(on, what, n + ' argument' + (n > 1 ? 's' : '') + ' required, but only ' + count + ' present.'); };

  // header blocks ("Name: value\r\n" lines): the value of one header, several lines joined as a Headers object would
  function blockGet(block, name) {
    var out = null, lines = String(block).split('\r\n');
    for (var i = 0; i < lines.length; i++) {
      var c = lines[i].indexOf(':');
      if (c > 0 && lines[i].slice(0, c).trim().toLowerCase() === name) { var v = lines[i].slice(c + 1).trim(); out = out === null ? v : out + ', ' + v; }
    }
    return out;
  }
  function urlKey(url, ignoreSearch) {
    var h = url.indexOf('#');
    if (h >= 0) url = url.slice(0, h);
    if (ignoreSearch) { var q = url.indexOf('?'); if (q >= 0) url = url.slice(0, q); }
    return url;
  }
  function options(o) {
    o = o || {};
    return { ignoreSearch: !!o.ignoreSearch, ignoreMethod: !!o.ignoreMethod, ignoreVary: !!o.ignoreVary, cacheName: o.cacheName === undefined ? null : String(o.cacheName) };
  }
  // does the stored pair answer the request?
  function matches(entry, request, opts) {
    if (!opts.ignoreMethod && request.method !== 'GET') return false;
    if (urlKey(entry.url, opts.ignoreSearch) !== urlKey(request.url, opts.ignoreSearch)) return false;
    if (opts.ignoreVary) return true;
    var vary = blockGet(entry.headers, 'vary');
    if (vary === null) return true;
    var names = vary.split(',');
    for (var i = 0; i < names.length; i++) {
      var n = names[i].trim().toLowerCase();
      if (!n) continue;
      if (n === '*') return false;
      var stored = blockGet(entry.requestHeaders, n), incoming = request.headers.get(n);
      if (stored !== incoming) return false;
    }
    return true;
  }
  function toRequest(input) { return input instanceof g.Request ? input : new g.Request(input); }
  // the indexes in cache `name` of the pairs that answer the request (the first only when `first`)
  function search(name, request, opts, first) {
    var n = __cache_count(name), hits = [];
    for (var i = 0; i < n; i++) {
      var e = __cache_entry(name, i, false);
      if (e && (request === null || matches(e, request, opts))) { hits.push(i); if (first) break; }
    }
    return hits;
  }
  function responseOf(name, index) {
    var e = __cache_entry(name, index, true);
    if (!e) return undefined;
    return g.__new_response(e.responseType || 'default', e.status, e.statusText, e.headers, e.body, e.responseUrl, e.redirected);
  }
  function requestOf(name, index) {
    var e = __cache_entry(name, index, false);
    if (!e) return null;
    var r = new g.Request(e.url, { method: e.method, headers: g.Headers.__fromBlock(e.requestHeaders) });
    return r;
  }
  function checkable(request, what, allowAnyMethod) {
    var url = new g.URL(request.url);
    if (url.protocol !== 'http:' && url.protocol !== 'https:') throw fail('Cache', what, 'Request scheme \'' + url.protocol.slice(0, -1) + '\' is unsupported');
    if (!allowAnyMethod && request.method !== 'GET') throw fail('Cache', what, 'Request method \'' + request.method + '\' is unsupported');
  }
  function quota() { return new DOMException('Quota exceeded.', 'QuotaExceededError'); }

  // the bytes of a response body, as a binary string
  function bytesOf(response) {
    if (response.type === 'opaque' || response.type === 'opaqueredirect') return Promise.resolve('');
    return response.clone().arrayBuffer().then(function (buffer) { return __bytes_to_string(new Uint8Array(buffer)); });
  }
  function entryOf(request, response, bytes) {
    return { method: request.method, url: urlKey(request.url, false), requestHeaders: request.headers.__block(), status: response.status, statusText: response.statusText,
             headers: response.headers.__block(), body: bytes, responseUrl: response.url, responseType: response.type, redirected: response.redirected };
  }
  function checkResponse(response, what) {
    if (response.status === 206) throw fail('Cache', what, 'Partial response (status code 206) is unsupported');
    var vary = response.headers.get('vary');
    if (vary !== null && vary.split(',').some(function (v) { return v.trim() === '*'; })) throw fail('Cache', what, 'Vary header contains *');
    if (response.bodyUsed) throw fail('Cache', what, 'Response body is already used');
  }
  // replaces what the request matches (ignoring nothing but the fragment) with the new pair, which goes last
  function store(name, request, entry) {
    var hits = search(name, request, { ignoreSearch: false, ignoreMethod: false, ignoreVary: false }, false);
    var code = __cache_write(name, -1, entry);
    if (code === 2) throw quota();
    for (var i = hits.length - 1; i >= 0; i--) __cache_erase(name, hits[i]);
    return code;
  }

  function newCache(name) {
    var c = Object.create(Cache.prototype);
    hide(c, '__name', name);
    return c;
  }
  method(Cache.prototype, 'match', function (request, o) {
    needArgs('Cache', 'match', 1, arguments.length);
    try {
      var r = toRequest(request), opts = options(o), name = this.__name;
      var hits = search(name, r, opts, true);
      return Promise.resolve(hits.length ? responseOf(name, hits[0]) : undefined);
    } catch (e) { return Promise.reject(e); }
  });
  method(Cache.prototype, 'matchAll', function (request, o) {
    try {
      var name = this.__name, opts = options(o), hits = request === undefined ? search(name, null, opts, false) : search(name, toRequest(request), opts, false);
      return Promise.resolve(hits.map(function (i) { return responseOf(name, i); }));
    } catch (e) { return Promise.reject(e); }
  });
  method(Cache.prototype, 'put', function (request, response) {
    var name = this.__name;
    try {
      needArgs('Cache', 'put', 2, arguments.length);
      var r = toRequest(request);
      if (!(response instanceof g.Response)) throw fail('Cache', 'put', "parameter 2 is not of type 'Response'.");
      checkable(r, 'put', false);
      checkResponse(response, 'put');
    } catch (e) { return Promise.reject(e); }
    return bytesOf(response).then(function (bytes) { store(name, r, entryOf(r, response, bytes)); });
  });
  function addAll(name, requests, what) {
    var list;
    try {
      list = requests.map(function (x) { var r = toRequest(x); checkable(r, what, false); return new g.Request(r); });
      for (var i = 0; i < list.length; i++) for (var k = 0; k < i; k++)
        if (urlKey(list[i].url, false) === urlKey(list[k].url, false)) throw new DOMException("Failed to execute '" + what + "' on 'Cache': duplicate requests", 'InvalidStateError');
    } catch (e) { return Promise.reject(e); }
    return Promise.all(list.map(function (r) { return g.fetch(r); })).then(function (responses) {
      for (var i = 0; i < responses.length; i++) {
        var res = responses[i];
        if (!(res.status >= 200 && res.status <= 299) && res.type !== 'opaque') throw fail('Cache', what, 'Request failed');
        checkResponse(res, what);
      }
      return Promise.all(responses.map(bytesOf)).then(function (all) {
        var entries = list.map(function (r, i) { return entryOf(r, responses[i], all[i]); });
        // all or nothing: what was written is taken back when a later write fails
        var written = [];
        try {
          for (var i = 0; i < entries.length; i++) { store(name, list[i], entries[i]); written.push(entries[i]); }
        } catch (e) {
          written.forEach(function (w) { search(name, new g.Request(w.url), { ignoreSearch: false, ignoreMethod: true, ignoreVary: true }, false).reverse().forEach(function (idx) { __cache_erase(name, idx); }); });
          throw e;
        }
      });
    });
  }
  method(Cache.prototype, 'add', function (request) { needArgs('Cache', 'add', 1, arguments.length); return addAll(this.__name, [request], 'add'); });
  method(Cache.prototype, 'addAll', function (requests) {
    needArgs('Cache', 'addAll', 1, arguments.length);
    var list;
    try { list = Array.prototype.slice.call(requests); } catch (e) { return Promise.reject(fail('Cache', 'addAll', "The provided value cannot be converted to a sequence.")); }
    return addAll(this.__name, list, 'addAll');
  });
  method(Cache.prototype, 'delete', function (request, o) {
    needArgs('Cache', 'delete', 1, arguments.length);
    try {
      var r = toRequest(request), name = this.__name, hits = search(name, r, options(o), false);
      for (var i = hits.length - 1; i >= 0; i--) __cache_erase(name, hits[i]);
      return Promise.resolve(hits.length > 0);
    } catch (e) { return Promise.reject(e); }
  });
  method(Cache.prototype, 'keys', function (request, o) {
    try {
      var name = this.__name, opts = options(o), hits = request === undefined ? search(name, null, opts, false) : search(name, toRequest(request), opts, false);
      return Promise.resolve(hits.map(function (i) { return requestOf(name, i); }));
    } catch (e) { return Promise.reject(e); }
  });

  var all = {};
  var storage = Object.create(CacheStorage.prototype);
  var cacheFor = function (name) { return all[name] || (all[name] = newCache(name)); };
  method(CacheStorage.prototype, 'open', function (name) {
    needArgs('CacheStorage', 'open', 1, arguments.length);
    name = String(name);
    if (!__cache_open(name)) return Promise.reject(new DOMException('Unexpected internal error.', 'UnknownError'));
    return Promise.resolve(cacheFor(name));
  });
  method(CacheStorage.prototype, 'has', function (name) { needArgs('CacheStorage', 'has', 1, arguments.length); return Promise.resolve(__cache_has(String(name))); });
  method(CacheStorage.prototype, 'delete', function (name) {
    needArgs('CacheStorage', 'delete', 1, arguments.length);
    name = String(name);
    var had = __cache_delete(name);
    delete all[name];
    return Promise.resolve(had);
  });
  method(CacheStorage.prototype, 'keys', function () { return Promise.resolve(__cache_names()); });
  method(CacheStorage.prototype, 'match', function (request, o) {
    needArgs('CacheStorage', 'match', 1, arguments.length);
    try {
      var r = toRequest(request), opts = options(o), names = opts.cacheName !== null ? (__cache_has(opts.cacheName) ? [opts.cacheName] : []) : __cache_names();
      for (var i = 0; i < names.length; i++) {
        var hits = search(names[i], r, opts, true);
        if (hits.length) return Promise.resolve(responseOf(names[i], hits[0]));
      }
      return Promise.resolve(undefined);
    } catch (e) { return Promise.reject(e); }
  });
  Object.defineProperty(g, 'caches', { get: function () { return storage; }, enumerable: true, configurable: true });
}(this));
)JSPRELUDE";

const char kClientPrelude[] = R"JSPRELUDE((function (g) {
  if (typeof __sw_available !== 'function') return;
  var hide = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true }); };
  var method = function (proto, name, fn) { Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true }); };
  var tag = function (ctor, name) { try { Object.defineProperty(ctor.prototype, Symbol.toStringTag, { value: name, configurable: true }); } catch (e) { } };
  var accessor = function (proto, name, get) { Object.defineProperty(proto, name, { get: get, enumerable: true, configurable: true }); };
  var ET = g.EventTarget;
  var iface = function (name, parent) {
    var f = function () { throw new TypeError('Illegal constructor'); };
    Object.defineProperty(f, 'name', { value: name });
    f.prototype = Object.create(parent ? parent.prototype : Object.prototype);
    Object.defineProperty(f.prototype, 'constructor', { value: f, writable: true, configurable: true });
    tag(f, name);
    if (!(name in g)) hide(g, name, f);
    return f;
  };
  var inScope = typeof __sws_id === 'function' && __sws_id() !== 0;   // (the page is the global scope of a service worker)
  var ServiceWorker = iface('ServiceWorker', ET), ServiceWorkerRegistration = iface('ServiceWorkerRegistration', ET), ServiceWorkerContainer = iface('ServiceWorkerContainer', ET);
  var NavigationPreloadManager = iface('NavigationPreloadManager'), SyncManager = iface('SyncManager'), PeriodicSyncManager = iface('PeriodicSyncManager'), PushManager = iface('PushManager');
  var error = function (res) {
    var m = res.message || 'Failed';
    return res.name === 'TypeError' ? new TypeError(m) : new DOMException(m, res.name || 'InvalidStateError');
  };

  // ------------------------------------------------------------------------------------------------------------------------------ ServiceWorker
  var workerObjs = {};
  function worker(info) {
    if (!info) return null;
    var w = workerObjs[info.id];
    if (!w) {
      w = Object.create(ServiceWorker.prototype);
      hide(w, '__id', info.id); hide(w, '__state', info.state); hide(w, '__url', info.scriptURL); hide(w, '__reg', info.reg);
      w.onstatechange = null; w.onerror = null;
      workerObjs[info.id] = w;
    } else {
      w.__state = info.state;
    }
    return w;
  }
  accessor(ServiceWorker.prototype, 'scriptURL', function () { return this.__url; });
  accessor(ServiceWorker.prototype, 'state', function () {
    if (inScope && this.__id === __sws_id()) {   // (the worker's own object: asked of the hub, the events go to documents)
      var snap = JSON.parse(__sws_registration()), slots = ['installing', 'waiting', 'active'];
      for (var i = 0; i < 3; i++) if (snap[slots[i]] && snap[slots[i]].id === this.__id) return snap[slots[i]].state;
      return 'redundant';
    }
    return this.__state;
  });
  method(ServiceWorker.prototype, 'postMessage', function (message, transfer) {
    if (arguments.length < 1) throw new TypeError("Failed to execute 'postMessage' on 'ServiceWorker': 1 argument required, but only 0 present.");
    if (this.state === 'redundant') throw new DOMException("Failed to execute 'postMessage' on 'ServiceWorker': ServiceWorker is in redundant state.", 'InvalidStateError');
    var s = g.__serialize_transfer(message, transfer);
    __sw_post(this.__id, s.text, s.ids);
  });

  // ------------------------------------------------------------------------------------------------------------------------ ServiceWorkerRegistration
  var regObjs = {};
  function apply(r, snap) {
    r.__snap = snap;
    r.__installing = worker(snap.installing); r.__waiting = worker(snap.waiting); r.__active = worker(snap.active);
  }
  function registration(snap) {
    if (!snap) return null;
    var r = regObjs[snap.id];
    if (!r) {
      r = Object.create(ServiceWorkerRegistration.prototype);
      hide(r, '__id', snap.id); hide(r, '__snap', snap); hide(r, '__installing', null); hide(r, '__waiting', null); hide(r, '__active', null);
      hide(r, '__managers', {});
      r.onupdatefound = null;
      regObjs[snap.id] = r;
    }
    apply(r, snap);
    return r;
  }
  function fresh(r) {   // inside a worker nothing tells the objects about changes: they look again
    if (inScope) { var text = __sw_registration(r.__id); if (text) apply(r, JSON.parse(text)); }
    return r;
  }
  accessor(ServiceWorkerRegistration.prototype, 'scope', function () { return this.__snap.scope; });
  accessor(ServiceWorkerRegistration.prototype, 'updateViaCache', function () { return this.__snap.updateViaCache; });
  accessor(ServiceWorkerRegistration.prototype, 'installing', function () { return fresh(this).__installing; });
  accessor(ServiceWorkerRegistration.prototype, 'waiting', function () { return fresh(this).__waiting; });
  accessor(ServiceWorkerRegistration.prototype, 'active', function () { return fresh(this).__active; });
  method(ServiceWorkerRegistration.prototype, 'update', function () {
    var r = this, res = JSON.parse(__sw_update(r.__id));
    if (!res.ok) return Promise.reject(error(res));
    apply(r, res.reg);
    if (g.__sw_flush) g.__sw_flush();   // (what the update made - updatefound - is heard before it is answered)
    return Promise.resolve(r);
  });
  method(ServiceWorkerRegistration.prototype, 'unregister', function () { return Promise.resolve(__sw_unregister(this.__id)); });
  method(ServiceWorkerRegistration.prototype, 'showNotification', function (title, options) {
    if (arguments.length < 1) return Promise.reject(new TypeError("Failed to execute 'showNotification' on 'ServiceWorkerRegistration': 1 argument required, but only 0 present."));
    var sp;
    try { sp = g.__notification_spec(title, options); } catch (e) { return Promise.reject(e); }
    var res = JSON.parse(__nt_show(this.__id, sp.title, sp.body, sp.tag, sp.sticky, sp.renotify, sp.options, sp.ids, sp.titles));
    if (!res.ok) return Promise.reject(res.name === 'NotAllowedError' ? new TypeError("Failed to execute 'showNotification' on 'ServiceWorkerRegistration': " + res.message) : error(res));
    return Promise.resolve();
  });
  method(ServiceWorkerRegistration.prototype, 'getNotifications', function (filter) {
    var tag = filter && filter.tag !== undefined ? String(filter.tag) : '';
    return Promise.resolve(JSON.parse(__nt_list(this.__id, tag)).map(g.__notification_from));
  });
  function manager(r, key, make) { return r.__managers[key] || (r.__managers[key] = make(r)); }
  accessor(ServiceWorkerRegistration.prototype, 'navigationPreload', function () {
    return manager(this, 'preload', function (r) { var m = Object.create(NavigationPreloadManager.prototype); hide(m, '__reg', r); return m; });
  });
  var preloadCall = function (m, op, value) {
    var r = m.__reg;
    if (!r.__active && !inScope) return Promise.reject(new DOMException('The registration does not have an active worker.', 'InvalidStateError'));
    return Promise.resolve(JSON.parse(__sw_preload(r.__id, op, value === undefined ? '' : String(value))));
  };
  method(NavigationPreloadManager.prototype, 'enable', function () { return preloadCall(this, 'enable').then(function () { }); });
  method(NavigationPreloadManager.prototype, 'disable', function () { return preloadCall(this, 'disable').then(function () { }); });
  method(NavigationPreloadManager.prototype, 'setHeaderValue', function (value) {
    if (arguments.length < 1) return Promise.reject(new TypeError("Failed to execute 'setHeaderValue' on 'NavigationPreloadManager': 1 argument required, but only 0 present."));
    return preloadCall(this, 'header', value).then(function () { });
  });
  method(NavigationPreloadManager.prototype, 'getState', function () { return preloadCall(this, 'state').then(function (s) { return { enabled: s.enabled, headerValue: s.header }; }); });
  accessor(ServiceWorkerRegistration.prototype, 'sync', function () {
    return manager(this, 'sync', function (r) { var m = Object.create(SyncManager.prototype); hide(m, '__reg', r); return m; });
  });
  method(SyncManager.prototype, 'register', function (tag) {
    if (arguments.length < 1) return Promise.reject(new TypeError("Failed to execute 'register' on 'SyncManager': 1 argument required, but only 0 present."));
    if (!__sw_sync_register(this.__reg.__id, String(tag))) return Promise.reject(new DOMException('Failed to register sync: no active Service Worker.', 'InvalidStateError'));
    return Promise.resolve();
  });
  method(SyncManager.prototype, 'getTags', function () { return Promise.resolve(JSON.parse(__sw_sync_tags(this.__reg.__id))); });
  accessor(ServiceWorkerRegistration.prototype, 'periodicSync', function () {
    return manager(this, 'periodic', function (r) { var m = Object.create(PeriodicSyncManager.prototype); hide(m, '__reg', r); return m; });
  });
  method(PeriodicSyncManager.prototype, 'register', function () { return Promise.reject(new DOMException('Permission denied.', 'NotAllowedError')); });
  method(PeriodicSyncManager.prototype, 'getTags', function () { return Promise.resolve([]); });
  method(PeriodicSyncManager.prototype, 'unregister', function () { return Promise.resolve(); });
  accessor(ServiceWorkerRegistration.prototype, 'pushManager', function () {
    return manager(this, 'push', function (r) { var m = Object.create(PushManager.prototype); hide(m, '__reg', r); return m; });
  });
  // ------------------------------------------------------------------------------------------------------------------------------ Push
  var PushSubscription = iface('PushSubscription'), PushSubscriptionOptions = iface('PushSubscriptionOptions'), PushMessageData = iface('PushMessageData');
  var bytesOfHex = function (hex) {
    var out = new Uint8Array(hex.length >> 1);
    for (var i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
    return out;
  };
  var hexOfBytes = function (bytes) {
    var out = '';
    for (var i = 0; i < bytes.length; i++) out += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
    return out;
  };
  var base64url = function (bytes) {
    var bin = '';
    for (var i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
    return g.btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  };
  // the applicationServerKey of subscribe(): base64url text or bytes; a P-256 public key (65 bytes, the first is 4) as hex
  var keyHex = function (key) {
    var bytes;
    if (typeof key === 'string') {
      try { var bin = g.atob(key.replace(/-/g, '+').replace(/_/g, '/')); bytes = new Uint8Array(bin.length); for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i); }
      catch (e) { throw new DOMException("Failed to execute 'subscribe' on 'PushManager': The provided applicationServerKey is not encoded as base64url without padding.", 'InvalidCharacterError'); }
    } else if (key instanceof ArrayBuffer) bytes = new Uint8Array(key);
    else if (ArrayBuffer.isView(key)) bytes = new Uint8Array(key.buffer, key.byteOffset, key.byteLength);
    else throw new TypeError("Failed to execute 'subscribe' on 'PushManager': The provided value is not of type '(ArrayBuffer or ArrayBufferView or DOMString)'.");
    if (bytes.length !== 65 || bytes[0] !== 4) throw new DOMException("Failed to execute 'subscribe' on 'PushManager': The provided applicationServerKey is not valid.", 'InvalidAccessError');
    return hexOfBytes(bytes);
  };
  function subscription(info, reg) {
    var s = Object.create(PushSubscription.prototype), o = Object.create(PushSubscriptionOptions.prototype);
    Object.defineProperty(o, 'userVisibleOnly', { value: true, enumerable: true });
    Object.defineProperty(o, 'applicationServerKey', { value: info.key ? bytesOfHex(info.key).buffer : null, enumerable: true });
    Object.defineProperty(s, 'endpoint', { value: info.endpoint, enumerable: true });
    Object.defineProperty(s, 'expirationTime', { value: null, enumerable: true });
    Object.defineProperty(s, 'options', { value: o, enumerable: true });
    hide(s, '__info', info); hide(s, '__reg', reg);
    return s;
  }
  method(PushSubscription.prototype, 'getKey', function (name) {
    if (arguments.length < 1) throw new TypeError("Failed to execute 'getKey' on 'PushSubscription': 1 argument required, but only 0 present.");
    name = String(name);
    if (name === 'p256dh') return bytesOfHex(this.__info.p256dh).buffer;
    if (name === 'auth') return bytesOfHex(this.__info.auth).buffer;
    if (name === '') return null;
    throw new TypeError("Failed to execute 'getKey' on 'PushSubscription': The provided value '" + name + "' is not a valid enum value of type PushEncryptionKeyName.");
  });
  method(PushSubscription.prototype, 'toJSON', function () {
    return { endpoint: this.endpoint, expirationTime: null, keys: { p256dh: base64url(bytesOfHex(this.__info.p256dh)), auth: base64url(bytesOfHex(this.__info.auth)) } };
  });
  method(PushSubscription.prototype, 'unsubscribe', function () {
    var current = __pu_get(this.__reg);
    if (!current || JSON.parse(current).endpoint !== this.endpoint) return Promise.resolve(false);
    return Promise.resolve(__pu_unsubscribe(this.__reg));
  });
  var messageData = function (hex) {
    var d = Object.create(PushMessageData.prototype);
    hide(d, '__bytes', bytesOfHex(hex));
    return d;
  };
  method(PushMessageData.prototype, 'arrayBuffer', function () { return this.__bytes.slice().buffer; });
  method(PushMessageData.prototype, 'bytes', function () { return this.__bytes.slice(); });
  method(PushMessageData.prototype, 'blob', function () { return new g.Blob([this.__bytes.slice()]); });
  method(PushMessageData.prototype, 'text', function () { return new g.TextDecoder().decode(this.__bytes); });
  method(PushMessageData.prototype, 'json', function () { return JSON.parse(this.text()); });
  method(PushManager.prototype, 'subscribe', function (options) {
    var reg = this.__reg, key = '';
    options = options || {};
    try { if (options.applicationServerKey !== undefined && options.applicationServerKey !== null) key = keyHex(options.applicationServerKey); } catch (e) { return Promise.reject(e); }
    if (!options.userVisibleOnly) return Promise.reject(new DOMException('Failed to execute \'subscribe\' on \'PushManager\': Subscription failed - userVisibleOnly is false', 'NotAllowedError'));
    var asked = !inScope && __nt_permission() === 'default' ? g.Notification.requestPermission() : Promise.resolve();
    return asked.then(function () {
      var res = JSON.parse(__pu_subscribe(reg.__id, true, key));
      if (!res.ok) throw error(res);
      return subscription(res.sub, reg.__id);
    });
  });
  method(PushManager.prototype, 'getSubscription', function () {
    var text = __pu_get(this.__reg.__id);
    return Promise.resolve(text ? subscription(JSON.parse(text), this.__reg.__id) : null);
  });
  method(PushManager.prototype, 'permissionState', function () { return Promise.resolve(__pu_state(this.__reg.__id)); });
  PushManager.supportedContentEncodings = ['aes128gcm'];

  hide(g, '__sw_internals', { worker: worker, registration: registration });
  hide(g, '__push_data', messageData); hide(g, '__push_subscription', subscription);
  if (inScope || !g.navigator || g.isSecureContext === false) return;
  var N = g.navigator;

  // ------------------------------------------------------------------------------------------------------------------------ ServiceWorkerContainer
  var container = Object.create(ServiceWorkerContainer.prototype);
  container.oncontrollerchange = null; container.onmessage = null; container.onmessageerror = null;
  var controllerObj = null, controllerLoaded = false;
  accessor(ServiceWorkerContainer.prototype, 'controller', function () {
    if (!controllerLoaded) {
      controllerLoaded = true;
      var text = __sw_controller();
      controllerObj = text ? worker(JSON.parse(text)) : null;
    }
    return controllerObj;
  });
  var readyResolve = null, readyPromise = null;
  function checkReady() {
    if (!readyResolve) return;
    var text = __sw_get_registration('');
    if (!text) return;
    var snap = JSON.parse(text);
    if (snap.active) { var done = readyResolve; readyResolve = null; done(registration(snap)); }
  }
  accessor(ServiceWorkerContainer.prototype, 'ready', function () {
    if (!readyPromise) { readyPromise = new Promise(function (resolve) { readyResolve = resolve; }); checkReady(); }
    return readyPromise;
  });
  var absolute = function (what, text) {
    try { return new g.URL(String(text), g.location.href); } catch (e) { throw new TypeError("Failed to " + what + ": Invalid URL"); }
  };
  method(ServiceWorkerContainer.prototype, 'register', function (url, options) {
    var script, scope, type, via;
    try {
      if (arguments.length < 1) throw new TypeError("Failed to execute 'register' on 'ServiceWorkerContainer': 1 argument required, but only 0 present.");
      options = options || {};
      script = absolute("execute 'register' on 'ServiceWorkerContainer'", url);
      scope = options.scope !== undefined ? absolute("execute 'register' on 'ServiceWorkerContainer'", options.scope) : new g.URL('./', script.href);
      type = options.type === undefined ? 'classic' : String(options.type);
      via = options.updateViaCache === undefined ? 'imports' : String(options.updateViaCache);
      if (['classic', 'module'].indexOf(type) < 0) throw new TypeError("Failed to execute 'register' on 'ServiceWorkerContainer': The provided value '" + type + "' is not a valid enum value of type WorkerType.");
      if (['imports', 'all', 'none'].indexOf(via) < 0) throw new TypeError("Failed to execute 'register' on 'ServiceWorkerContainer': The provided value '" + via + "' is not a valid enum value of type ServiceWorkerUpdateViaCache.");
    } catch (e) { return Promise.reject(e); }
    if (!__sw_available()) return Promise.reject(new DOMException('Failed to register a ServiceWorker: The document is in an invalid state.', 'InvalidStateError'));
    var res = JSON.parse(__sw_register(script.href, scope.href, type, via));
    if (!res.ok) return Promise.reject(error(res));
    return Promise.resolve(registration(res.reg));
  });
  method(ServiceWorkerContainer.prototype, 'getRegistration', function (url) {
    try {
      var text = __sw_get_registration(url === undefined ? '' : absolute("execute 'getRegistration' on 'ServiceWorkerContainer'", url).href);
      return Promise.resolve(text ? registration(JSON.parse(text)) : undefined);
    } catch (e) { return Promise.reject(e); }
  });
  method(ServiceWorkerContainer.prototype, 'getRegistrations', function () { return Promise.resolve(JSON.parse(__sw_get_registrations()).map(registration)); });
  method(ServiceWorkerContainer.prototype, 'startMessages', function () { });
  Object.defineProperty(N, 'serviceWorker', { get: function () { return container; }, enumerable: true, configurable: true });

  // what the hub tells the document
  hide(g, '__sw_flush', function () {
    for (var guard = 0; guard < 64; guard++) {
      var text = __sw_take_event();
      if (!text) break;
      g.__sw_event(text);
    }
  });
  hide(g, '__sw_event', function (json) {
    var e = JSON.parse(json), r, w;
    switch (e.t) {
      case 'state':
        if (e.reg) registration(e.reg);
        w = workerObjs[e.worker];
        if (w) { w.__state = e.state; w.dispatchEvent(new g.Event('statechange')); }
        checkReady();
        break;
      case 'updatefound':
        r = registration(e.reg);
        r.dispatchEvent(new g.Event('updatefound'));
        break;
      case 'controllerchange':
        if (e.reg) registration(e.reg);
        controllerLoaded = true;
        controllerObj = e.reg && e.reg.active ? worker(e.reg.active) : null;
        container.dispatchEvent(new g.Event('controllerchange'));
        break;
      case 'message':
        var got;
        try { got = g.__deserialize_transfer(e.data, e.ports); } catch (x) { container.dispatchEvent(new g.MessageEvent('messageerror', { data: null })); break; }
        container.dispatchEvent(new g.MessageEvent('message', { data: got.data, origin: e.origin, lastEventId: '', source: workerObjs[e.worker] || null, ports: got.ports }));
        break;
      case 'permission':
        g.__notify_permission_changed(e.state);
        break;
      case 'notification':
        g.__notify_event(e.id, e.ev);
        break;
      case 'port':
        g.__port_deliver(e.port, e.data, e.ports);
        break;
      case 'navigate':
        g.location.href = e.url;
        break;
    }
  });
}(this));
)JSPRELUDE";

const char kScopePrelude[] = R"JSPRELUDE((function (g) {
  if (typeof __sws_id !== 'function' || __sws_id() === 0) return;
  var hide = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true }); };
  var method = function (proto, name, fn) { Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true }); };
  var tag = function (ctor, name) { try { Object.defineProperty(ctor.prototype, Symbol.toStringTag, { value: name, configurable: true }); } catch (e) { } };
  var accessor = function (proto, name, get) { Object.defineProperty(proto, name, { get: get, enumerable: true, configurable: true }); };
  var internals = g.__sw_internals;
  var typeErr = function (what, message) { return new TypeError("Failed to execute '" + what + "': " + message); };

  // events: new Name(type, init) builds on Event
  var mk = function (name, parent, fields, init) {
    var base = parent || g.Event;
    var C = function (type, options) {
      if (!(this instanceof C)) throw new TypeError("Failed to construct '" + name + "': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      if (arguments.length < 1) throw new TypeError("Failed to construct '" + name + "': 1 argument required, but only 0 present.");
      var e = new base(type, options);
      Object.setPrototypeOf(e, C.prototype);
      for (var i = 0; i < fields.length; i++) {
        var v = options && options[fields[i][0]] !== undefined ? options[fields[i][0]] : fields[i][1];
        Object.defineProperty(e, fields[i][0], { value: v, enumerable: true, writable: true, configurable: true });
      }
      if (init) init(e, options);
      return e;
    };
    Object.defineProperty(C, 'name', { value: name });
    C.prototype = Object.create(base.prototype);
    Object.defineProperty(C.prototype, 'constructor', { value: C, writable: true, configurable: true });
    tag(C, name);
    hide(g, name, C);
    return C;
  };
  var ExtendableEvent = mk('ExtendableEvent', null, [], function (e) { hide(e, '__promises', []); hide(e, '__open', false); hide(e, '__pending', 0); });
  method(ExtendableEvent.prototype, 'waitUntil', function (promise) {
    if (!this.__open && this.__pending === 0) throw new DOMException("Failed to execute 'waitUntil' on 'ExtendableEvent': The event handler is already finished and no extend lifetime promises are outstanding.", 'InvalidStateError');
    var self = this, p = Promise.resolve(promise);
    this.__pending++;
    this.__promises.push(p);
    var done = function () { self.__pending--; };
    p.then(done, done);
  });
  var InstallEvent = mk('InstallEvent', ExtendableEvent, []);
  var ExtendableMessageEvent = mk('ExtendableMessageEvent', ExtendableEvent, [['data', null], ['origin', ''], ['lastEventId', ''], ['source', null], ['ports', []]]);
  var FetchEvent = mk('FetchEvent', ExtendableEvent, [['request', null], ['clientId', ''], ['resultingClientId', ''], ['replacesClientId', ''], ['preloadResponse', null], ['handled', null]],
    function (e, o) {
      if (!o || !(o.request instanceof g.Request)) throw new TypeError("Failed to construct 'FetchEvent': Failed to read the 'request' property from 'FetchEventInit': Required member is undefined.");
      if (e.preloadResponse === null) e.preloadResponse = Promise.resolve(undefined);
      hide(e, '__responded', false); hide(e, '__answer', null);
    });
  method(FetchEvent.prototype, 'respondWith', function (response) {
    if (!this.__open) throw new DOMException("Failed to execute 'respondWith' on 'FetchEvent': The event handler is already finished.", 'InvalidStateError');
    if (this.__responded) throw new DOMException("Failed to execute 'respondWith' on 'FetchEvent': The FetchEvent has already been responded to.", 'InvalidStateError');
    this.__responded = true;
    this.__answer = Promise.resolve(response);
    this.waitUntil(this.__answer);
    this.preventDefault();
  });
  mk('PushEvent', ExtendableEvent, [['data', null]]);
  mk('SyncEvent', ExtendableEvent, [['tag', ''], ['lastChance', false]]);
  mk('PeriodicSyncEvent', ExtendableEvent, [['tag', '']]);
  mk('NotificationEvent', ExtendableEvent, [['notification', null], ['action', '']]);
  mk('PushSubscriptionChangeEvent', ExtendableEvent, [['oldSubscription', null], ['newSubscription', null]]);
  mk('BackgroundFetchUpdateUIEvent', ExtendableEvent, []);
  mk('CookieChangeEvent2', ExtendableEvent, []);

  // ------------------------------------------------------------------------------------------------------------------------------ clients
  var iface = function (name, parent) {
    var f = function () { throw new TypeError('Illegal constructor'); };
    Object.defineProperty(f, 'name', { value: name });
    f.prototype = Object.create(parent ? parent.prototype : Object.prototype);
    Object.defineProperty(f.prototype, 'constructor', { value: f, writable: true, configurable: true });
    tag(f, name);
    hide(g, name, f);
    return f;
  };
  var Client = iface('Client'), WindowClient = iface('WindowClient', Client), Clients = iface('Clients');
  var openAllowed = false;
  function clientObject(info) {
    if (!info) return null;
    var c = Object.create(info.type === 'window' ? WindowClient.prototype : Client.prototype);
    Object.defineProperty(c, 'id', { value: info.id, enumerable: true }); Object.defineProperty(c, 'url', { value: info.url, enumerable: true });
    Object.defineProperty(c, 'type', { value: info.type, enumerable: true }); Object.defineProperty(c, 'frameType', { value: info.frameType, enumerable: true });
    if (info.type === 'window') {
      Object.defineProperty(c, 'visibilityState', { value: info.visibilityState, enumerable: true }); Object.defineProperty(c, 'focused', { value: info.focused, enumerable: true });
      Object.defineProperty(c, 'ancestorOrigins', { value: [], enumerable: true });
    }
    return c;
  }
  method(Client.prototype, 'postMessage', function (message, transfer) {
    if (arguments.length < 1) throw typeErr('postMessage on Client', '1 argument required, but only 0 present.');
    var s = g.__serialize_transfer(message, transfer);
    __sws_client_post(this.id, s.text, s.ids);
  });
  method(WindowClient.prototype, 'focus', function () { return Promise.resolve(this); });
  method(WindowClient.prototype, 'navigate', function (url) {
    var self = this;
    var resolved = g.__url_resolve(String(url), g.location.href);
    if (resolved === null) return Promise.reject(new TypeError("Failed to execute 'navigate' on 'WindowClient': Invalid URL"));
    var text = __sws_client_navigate(self.id, resolved);
    return text ? Promise.resolve(clientObject(JSON.parse(text))) : Promise.reject(new TypeError("Failed to execute 'navigate' on 'WindowClient': Cannot navigate to URL: " + resolved));
  });
  var clients = Object.create(Clients.prototype);
  method(Clients.prototype, 'get', function (id) {
    if (arguments.length < 1) return Promise.reject(typeErr('get on Clients', '1 argument required, but only 0 present.'));
    var text = __sws_client(String(id));
    return Promise.resolve(text ? clientObject(JSON.parse(text)) : undefined);
  });
  method(Clients.prototype, 'matchAll', function (options) {
    options = options || {};
    var list = JSON.parse(__sws_clients(!!options.includeUncontrolled, options.type === undefined ? 'window' : String(options.type)));
    return Promise.resolve(list.map(clientObject));
  });
  method(Clients.prototype, 'openWindow', function (url) {
    if (arguments.length < 1) return Promise.reject(typeErr('openWindow on Clients', '1 argument required, but only 0 present.'));
    var resolved = g.__url_resolve(String(url), g.location.href);
    if (resolved === null) return Promise.reject(new TypeError("Failed to execute 'openWindow' on 'Clients': 'url' is invalid."));
    if (!openAllowed) return Promise.reject(new DOMException('Not allowed to open a window.', 'InvalidAccessError'));
    __sws_open_window(resolved);
    return Promise.resolve(null);
  });
  method(Clients.prototype, 'claim', function () {
    var reg = internals.registration(JSON.parse(__sws_registration()));
    if (!reg.active || reg.active.state === 'redundant' || reg.active.state === 'installing' || reg.active.state === 'installed' || reg.active.__id !== __sws_id())
      return Promise.reject(new DOMException('Failed to claim clients: The service worker is not active.', 'InvalidStateError'));
    __sws_claim();
    return Promise.resolve();
  });

  // ------------------------------------------------------------------------------------------------------------------------------ the global scope
  var ServiceWorkerGlobalScope = function () { throw new TypeError('Illegal constructor'); };
  ServiceWorkerGlobalScope.prototype = Object.getPrototypeOf(g);
  tag(ServiceWorkerGlobalScope, 'ServiceWorkerGlobalScope');
  hide(g, 'ServiceWorkerGlobalScope', ServiceWorkerGlobalScope);
  var define = function (name, get) { Object.defineProperty(g, name, { get: get, enumerable: true, configurable: true }); };
  var snapshot = function () { return internals.registration(JSON.parse(__sws_registration())); };
  define('registration', snapshot);
  define('serviceWorker', function () { var r = snapshot(); return r.active && r.active.__id === __sws_id() ? r.active : r.waiting && r.waiting.__id === __sws_id() ? r.waiting : r.installing; });
  define('clients', function () { return clients; });
  hide(g, 'skipWaiting', function () { __sws_skip_waiting(); return Promise.resolve(); });
  ['oninstall', 'onactivate', 'onfetch', 'onmessage', 'onmessageerror', 'onsync', 'onperiodicsync', 'onpush', 'onpushsubscriptionchange', 'onnotificationclick', 'onnotificationclose'].forEach(function (name) {
    if (g[name] === undefined) g[name] = null;
  });
  hide(g, 'importScripts', function () {
    for (var i = 0; i < arguments.length; i++) {
      var url = g.__url_resolve(String(arguments[i]), g.location.href);
      if (url === null) throw new DOMException("Failed to execute 'importScripts' on 'WorkerGlobalScope': The URL '" + arguments[i] + "' is invalid.", 'SyntaxError');
      var source = __sws_import(url);
      if (source === null) throw new DOMException("Failed to execute 'importScripts' on 'WorkerGlobalScope': The script at '" + url + "' failed to load.", 'NetworkError');
      (0, eval)(source);
    }
  });

  // ------------------------------------------------------------------------------------------------------------------------------ dispatching
  function settle(event, token) {
    var count = event.__promises.length;
    Promise.all(event.__promises.slice()).then(function () {
      if (event.__promises.length > count) return settle(event, token);
      __sws_done(token, true, '');
    }, function (e) { __sws_done(token, false, String(e && e.message !== undefined ? (e.name || 'Error') + ': ' + e.message : e)); });
  }
  function fire(event) {
    event.__open = true;
    try { g.dispatchEvent(event); } finally { event.__open = false; }
  }
  hide(g, '__sw_dispatch', function (type, token, argJson) {
    var arg = argJson ? JSON.parse(argJson) : {}, event;
    switch (type) {
      case 'install': event = new InstallEvent('install'); break;
      case 'activate': event = new ExtendableEvent('activate'); break;
      case 'message':
        var got;
        try { got = g.__deserialize_transfer(arg.data, arg.ports); } catch (e) { __sws_done(token, false, 'The message could not be deserialized'); return; }
        var info = arg.source ? __sws_client(arg.source) : '';
        event = new ExtendableMessageEvent('message', { data: got.data, origin: arg.origin || '', lastEventId: '', source: info ? clientObject(JSON.parse(info)) : null, ports: got.ports });
        break;
      case 'sync': event = new g.SyncEvent('sync', { tag: arg.tag, lastChance: !!arg.lastChance }); break;
      case 'push': event = new g.PushEvent('push', { data: arg.data === undefined ? null : g.__push_data(arg.data) }); break;
      case 'pushsubscriptionchange': event = new g.PushSubscriptionChangeEvent('pushsubscriptionchange', { oldSubscription: arg.old ? g.__push_subscription(arg.old, JSON.parse(__sws_registration()).id) : null, newSubscription: null }); break;
      case 'notificationclose': event = new g.NotificationEvent('notificationclose', { notification: arg.notification ? g.__notification_from(arg.notification) : null, action: arg.action || '' }); break;
      case 'notificationclick': openAllowed = true; event = new g.NotificationEvent('notificationclick', { notification: arg.notification ? g.__notification_from(arg.notification) : null, action: arg.action || '' }); break;
      default: __sws_done(token, true, ''); return;
    }
    try { fire(event); } finally { openAllowed = false; }
    settle(event, token);
  });
  function buildRequest(info, body) {
    var init = { method: info.method, headers: g.Headers.__fromBlock(info.headers), mode: info.mode === 'navigate' ? 'same-origin' : info.mode, credentials: info.credentials, redirect: info.redirect };
    if (body && info.method !== 'GET' && info.method !== 'HEAD') init.body = body;
    var request;
    try { request = new g.Request(info.url, init); } catch (e) { init.mode = 'cors'; request = new g.Request(info.url, init); }
    request.__q.mode = info.mode;
    request.__q.destination = info.destination || '';
    if (info.referrer) request.__q.referrer = info.referrer;
    return request;
  }
  var NETWORK_ERROR = function (token) { __sws_answer(token, 2, 0, '', '', '', '', '', false); };
  hide(g, '__sw_dispatch_fetch', function (token, infoJson, body) {
    var info = JSON.parse(infoJson), request;
    try { request = buildRequest(info, body); } catch (e) { __sws_answer(token, 0, 0, '', '', '', '', '', false); return; }
    var preload;
    if (info.preload) preload = g.fetch(info.url, { headers: { 'Service-Worker-Navigation-Preload': info.preloadHeader } }).then(function (r) { return r; }, function () { return undefined; });
    else preload = Promise.resolve(undefined);
    var event = new FetchEvent('fetch', { request: request, clientId: info.client || '', resultingClientId: info.navigation ? info.client || '' : '', preloadResponse: preload, cancelable: true });
    fire(event);
    if (!event.__responded) { __sws_answer(token, 0, 0, '', '', '', '', '', false); return; }
    event.__answer.then(function (r) {
      if (!(r instanceof g.Response) || r.type === 'error' || r.type === 'opaqueredirect' || r.bodyUsed || (request.mode === 'cors' && r.type === 'opaque') || (request.mode === 'same-origin' && (r.type === 'cors' || r.type === 'opaque'))) { NETWORK_ERROR(token); return; }
      if (r.type === 'opaque') { __sws_answer(token, 1, 0, '', '', '', r.url, 'opaque', false); return; }
      r.arrayBuffer().then(function (buffer) {
        __sws_answer(token, 1, r.status, r.statusText, r.headers.__block(), __bytes_to_string(new Uint8Array(buffer)), r.url, r.type, r.redirected);
      }, function () { NETWORK_ERROR(token); });
    }, function () { NETWORK_ERROR(token); });
  });
}(this));
)JSPRELUDE";

}  // namespace

const char* cache_prelude() { return kCachePrelude; }
const char* service_worker_client_prelude() { return kClientPrelude; }
const char* service_worker_scope_prelude() { return kScopePrelude; }

}  // namespace browser
