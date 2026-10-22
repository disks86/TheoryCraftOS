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

#include "Browser/WebPrelude.hpp"

namespace browser {

namespace {

const char kPrelude[] = R"JSPRELUDE((function (g) {
  var hide = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true }); };
  var method = function (proto, name, fn) { Object.defineProperty(proto, name, { value: fn, writable: true }); };
  var ITER = Symbol.iterator;
  var typeError = function (what, message) { return new TypeError("Failed to " + what + ": " + message); };
  var lower = function (s) { return String(s).toLowerCase(); };
  var isObject = function (v) { return v !== null && typeof v === 'object'; };

  // ---------------------------------------------------------------------------------------------------------------- URLSearchParams
  function parseQuery(list, text) {
    if (text.charAt(0) === '?') text = text.slice(1);
    var pieces = text.split('&');
    for (var i = 0; i < pieces.length; i++) {
      var piece = pieces[i];
      if (piece === '') continue;
      var eq = piece.indexOf('=');
      list.push([__form_decode(eq < 0 ? piece : piece.slice(0, eq)), __form_decode(eq < 0 ? '' : piece.slice(eq + 1))]);
    }
  }
  function serializeQuery(list) {
    var out = [];
    for (var i = 0; i < list.length; i++) out.push(__form_encode(list[i][0]) + '=' + __form_encode(list[i][1]));
    return out.join('&');
  }
  function paramsChanged(sp) {
    sp.size = sp.__list.length;
    if (sp.__url) sp.__url.search = serializeQuery(sp.__list);
  }
  function URLSearchParams(init) {
    if (!(this instanceof URLSearchParams)) throw typeError("construct 'URLSearchParams'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    hide(this, '__list', []);
    hide(this, '__url', null);
    hide(this, 'size', 0);
    if (init === undefined || init === null) return;
    var list = this.__list;
    if (init instanceof URLSearchParams) {
      for (var i = 0; i < init.__list.length; i++) list.push([init.__list[i][0], init.__list[i][1]]);
    } else if (Array.isArray(init)) {
      for (var j = 0; j < init.length; j++) {
        var pair = init[j];
        if (!Array.isArray(pair) || pair.length !== 2) throw typeError("construct 'URLSearchParams'", "Each query pair must be an iterable [name, value] tuple");
        list.push([String(pair[0]), String(pair[1])]);
      }
    } else if (typeof init === 'object') {
      var keys = Object.keys(init);
      for (var k = 0; k < keys.length; k++) list.push([keys[k], String(init[keys[k]])]);
    } else {
      parseQuery(list, String(init));
    }
    this.size = list.length;
  }
  var SP = URLSearchParams.prototype;
  method(SP, '__reset', function (search) { this.__list.length = 0; parseQuery(this.__list, search); this.size = this.__list.length; });
  method(SP, 'append', function (name, value) {
    if (arguments.length < 2) throw typeError("execute 'append' on 'URLSearchParams'", "2 arguments required, but only " + arguments.length + " present.");
    this.__list.push([String(name), String(value)]); paramsChanged(this);
  });
  method(SP, 'delete', function (name, value) {
    name = String(name);
    var list = this.__list, kept = [];
    for (var i = 0; i < list.length; i++) if (!(list[i][0] === name && (value === undefined || list[i][1] === String(value)))) kept.push(list[i]);
    this.__list = kept; paramsChanged(this);
  });
  method(SP, 'get', function (name) {
    name = String(name);
    for (var i = 0; i < this.__list.length; i++) if (this.__list[i][0] === name) return this.__list[i][1];
    return null;
  });
  method(SP, 'getAll', function (name) {
    name = String(name);
    var out = [];
    for (var i = 0; i < this.__list.length; i++) if (this.__list[i][0] === name) out.push(this.__list[i][1]);
    return out;
  });
  method(SP, 'has', function (name, value) {
    name = String(name);
    for (var i = 0; i < this.__list.length; i++) if (this.__list[i][0] === name && (value === undefined || this.__list[i][1] === String(value))) return true;
    return false;
  });
  method(SP, 'set', function (name, value) {
    name = String(name); value = String(value);
    var list = this.__list, out = [], done = false;
    for (var i = 0; i < list.length; i++) {
      if (list[i][0] !== name) { out.push(list[i]); continue; }
      if (!done) { out.push([name, value]); done = true; }
    }
    if (!done) out.push([name, value]);
    this.__list = out; paramsChanged(this);
  });
  method(SP, 'sort', function () {
    var list = this.__list;
    for (var i = 1; i < list.length; i++) {   // (a stable insertion sort on the names)
      var item = list[i], j = i - 1;
      while (j >= 0 && list[j][0] > item[0]) { list[j + 1] = list[j]; j--; }
      list[j + 1] = item;
    }
    paramsChanged(this);
  });
  method(SP, 'toString', function () { return serializeQuery(this.__list); });
  method(SP, 'forEach', function (callback, thisArg) {
    if (typeof callback !== 'function') throw typeError("execute 'forEach' on 'URLSearchParams'", "parameter 1 is not of type 'Function'.");
    for (var i = 0; i < this.__list.length; i++) callback.call(thisArg, this.__list[i][1], this.__list[i][0], this);
  });
  method(SP, 'keys', function () { return this.__list.map(function (p) { return p[0]; }); });
  method(SP, 'values', function () { return this.__list.map(function (p) { return p[1]; }); });
  method(SP, 'entries', function () { return this.__list.map(function (p) { return [p[0], p[1]]; }); });
  method(SP, ITER, function () { return this.entries(); });
  hide(g, '__make_search_params', function (url) {
    var sp = new URLSearchParams();
    sp.__url = url;
    parseQuery(sp.__list, url.search);
    sp.size = sp.__list.length;
    return sp;
  });
  hide(g, 'URLSearchParams', URLSearchParams);

  // ------------------------------------------------------------------------------------------------------------------------ Headers
  var TOKEN = "!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  function validName(name) {
    if (name.length === 0) return false;
    for (var i = 0; i < name.length; i++) if (TOKEN.indexOf(name.charAt(i)) < 0) return false;
    return true;
  }
  function cleanValue(value) {
    value = String(value);
    var start = 0, end = value.length;
    while (start < end && ' \t\r\n'.indexOf(value.charAt(start)) >= 0) start++;
    while (end > start && ' \t\r\n'.indexOf(value.charAt(end - 1)) >= 0) end--;
    value = value.slice(start, end);
    if (value.indexOf('\0') >= 0 || value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0) return null;
    return value;
  }
  function Headers(init) {
    if (!(this instanceof Headers)) throw typeError("construct 'Headers'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    hide(this, '__list', []);
    hide(this, '__immutable', false);
    if (init === undefined || init === null) return;
    if (init instanceof Headers) {
      for (var i = 0; i < init.__list.length; i++) this.__list.push([init.__list[i][0], init.__list[i][1]]);
    } else if (Array.isArray(init)) {
      for (var j = 0; j < init.length; j++) {
        if (!Array.isArray(init[j]) || init[j].length !== 2) throw typeError("construct 'Headers'", "Invalid value");
        this.append(init[j][0], init[j][1]);
      }
    } else if (typeof init === 'object') {
      var keys = Object.keys(init);
      for (var k = 0; k < keys.length; k++) this.append(keys[k], init[keys[k]]);
    } else {
      throw typeError("construct 'Headers'", "The provided value is not of type '(record<ByteString, ByteString> or sequence<sequence<ByteString>>)'.");
    }
  }
  var H = Headers.prototype;
  function checkHeader(what, headers, name, value) {
    name = String(name);
    if (!validName(name)) throw typeError("execute '" + what + "' on 'Headers'", "Invalid name");
    if (headers.__immutable) throw typeError("execute '" + what + "' on 'Headers'", "Headers are immutable");
    return name;
  }
  method(H, 'append', function (name, value) {
    name = checkHeader('append', this, name);
    var v = cleanValue(value);
    if (v === null) throw typeError("execute 'append' on 'Headers'", "Invalid value");
    this.__list.push([lower(name), v]);
  });
  method(H, 'set', function (name, value) {
    name = lower(checkHeader('set', this, name));
    var v = cleanValue(value);
    if (v === null) throw typeError("execute 'set' on 'Headers'", "Invalid value");
    var out = [], done = false;
    for (var i = 0; i < this.__list.length; i++) {
      if (this.__list[i][0] !== name) { out.push(this.__list[i]); continue; }
      if (!done) { out.push([name, v]); done = true; }
    }
    if (!done) out.push([name, v]);
    this.__list = out;
  });
  method(H, 'delete', function (name) {
    name = lower(checkHeader('delete', this, name));
    this.__list = this.__list.filter(function (p) { return p[0] !== name; });
  });
  method(H, 'get', function (name) {
    name = String(name);
    if (!validName(name)) throw typeError("execute 'get' on 'Headers'", "Invalid name");
    name = lower(name);
    var found = [];
    for (var i = 0; i < this.__list.length; i++) if (this.__list[i][0] === name) found.push(this.__list[i][1]);
    return found.length ? found.join(', ') : null;
  });
  method(H, 'getSetCookie', function () {
    var out = [];
    for (var i = 0; i < this.__list.length; i++) if (this.__list[i][0] === 'set-cookie') out.push(this.__list[i][1]);
    return out;
  });
  method(H, 'has', function (name) {
    name = String(name);
    if (!validName(name)) throw typeError("execute 'has' on 'Headers'", "Invalid name");
    name = lower(name);
    for (var i = 0; i < this.__list.length; i++) if (this.__list[i][0] === name) return true;
    return false;
  });
  // the entries as the standard lists them: sorted by name, the values of one name combined (but each Set-Cookie alone)
  function sortedEntries(headers) {
    var names = [];
    for (var i = 0; i < headers.__list.length; i++) if (names.indexOf(headers.__list[i][0]) < 0) names.push(headers.__list[i][0]);
    names.sort();
    var out = [];
    for (var n = 0; n < names.length; n++) {
      if (names[n] === 'set-cookie') {
        var cookies = headers.getSetCookie();
        for (var c = 0; c < cookies.length; c++) out.push([names[n], cookies[c]]);
      } else {
        out.push([names[n], headers.get(names[n])]);
      }
    }
    return out;
  }
  method(H, 'forEach', function (callback, thisArg) {
    if (typeof callback !== 'function') throw typeError("execute 'forEach' on 'Headers'", "parameter 1 is not of type 'Function'.");
    var entries = sortedEntries(this);
    for (var i = 0; i < entries.length; i++) callback.call(thisArg, entries[i][1], entries[i][0], this);
  });
  method(H, 'keys', function () { return sortedEntries(this).map(function (e) { return e[0]; }); });
  method(H, 'values', function () { return sortedEntries(this).map(function (e) { return e[1]; }); });
  method(H, 'entries', function () { return sortedEntries(this); });
  method(H, ITER, function () { return sortedEntries(this); });
  // "Name: value\r\n" lines for the network
  method(H, '__block', function () {
    var out = '';
    for (var i = 0; i < this.__list.length; i++) out += this.__list[i][0] + ': ' + this.__list[i][1] + '\r\n';
    return out;
  });
  Headers.__fromBlock = function (block) {
    var h = new Headers();
    var lines = block.split('\n');
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i];
      if (line.charAt(line.length - 1) === '\r') line = line.slice(0, line.length - 1);
      var colon = line.indexOf(':');
      if (colon <= 0) continue;
      var v = cleanValue(line.slice(colon + 1));
      if (v !== null) h.__list.push([lower(line.slice(0, colon).trim()), v]);
    }
    return h;
  };
  hide(g, 'Headers', Headers);

  // ------------------------------------------------------------------------------------------------------------------- TextEncoder / TextDecoder
  function TextEncoder() { this.encoding = 'utf-8'; }
  method(TextEncoder.prototype, 'encode', function (text) { return __string_to_bytes(text === undefined ? '' : String(text)); });
  method(TextEncoder.prototype, 'encodeInto', function (text, target) {
    var bytes = __string_to_bytes(String(text));
    var n = Math.min(bytes.length, target.length);
    for (var i = 0; i < n; i++) target[i] = bytes[i];
    return { read: text.length, written: n };
  });
  function TextDecoder(label, options) {
    label = label === undefined ? 'utf-8' : lower(String(label).trim());
    if (label === 'utf8' || label === 'unicode-1-1-utf-8') label = 'utf-8';
    if (label === 'latin1' || label === 'ascii' || label === 'us-ascii' || label === 'windows-1252' || label === 'iso-8859-1') label = 'windows-1252';
    if (label !== 'utf-8' && label !== 'windows-1252') throw new RangeError("Failed to construct 'TextDecoder': The encoding label provided ('" + label + "') is invalid.");
    this.encoding = label;
    this.fatal = !!(options && options.fatal);
    this.ignoreBOM = !!(options && options.ignoreBOM);
  }
  method(TextDecoder.prototype, 'decode', function (input) {
    if (input === undefined || input === null) return '';
    var bytes = __bytes_to_string(input);
    if (this.encoding === 'windows-1252') {
      var out = '';
      for (var i = 0; i < bytes.length; i++) {
        out += String.fromCharCode(bytes.charCodeAt(i));   // (strings hold UTF-8: a character code becomes its UTF-8 bytes)
      }
      return out;
    }
    if (!this.ignoreBOM && bytes.charCodeAt(0) === 0xEF && bytes.charCodeAt(1) === 0xBB && bytes.charCodeAt(2) === 0xBF) bytes = bytes.slice(3);
    var clean = __utf8_clean(bytes);
    if (this.fatal && clean !== bytes) throw new TypeError("Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid for encoding utf-8");
    return clean;
  });
  hide(g, 'TextEncoder', TextEncoder);
  hide(g, 'TextDecoder', TextDecoder);

  // ---------------------------------------------------------------------------------------------------------------------------- Blob, File
  function bytesOf(part) {
    if (part instanceof Blob) return part.__bytes;
    if (isObject(part) && typeof part.length === 'number' && typeof part.byteLength === 'number') return __bytes_to_string(part);
    return String(part);
  }
  function Blob(parts, options) {
    if (!(this instanceof Blob)) throw typeError("construct 'Blob'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    var bytes = '';
    if (parts !== undefined && parts !== null) {
      if (!Array.isArray(parts)) throw typeError("construct 'Blob'", "The provided value cannot be converted to a sequence.");
      for (var i = 0; i < parts.length; i++) bytes += bytesOf(parts[i]);
    }
    hide(this, '__bytes', bytes);
    this.size = bytes.length;
    var type = options && options.type !== undefined ? String(options.type) : '';
    this.type = /* lower-case, and empty if it holds anything but printable ASCII */ type.length && type.length === __utf8_clean(type).length ? lower(type) : '';
  }
  var B = Blob.prototype;
  method(B, 'text', function () { return Promise.resolve(__utf8_clean(this.__bytes)); });
  method(B, 'arrayBuffer', function () { return Promise.resolve(__string_to_bytes(this.__bytes)); });
  method(B, 'bytes', function () { return Promise.resolve(__string_to_bytes(this.__bytes)); });
  method(B, 'slice', function (start, end, type) {
    var size = this.__bytes.length;
    start = start === undefined ? 0 : Math.trunc(start);
    end = end === undefined ? size : Math.trunc(end);
    start = start < 0 ? Math.max(size + start, 0) : Math.min(start, size);
    end = end < 0 ? Math.max(size + end, 0) : Math.min(end, size);
    return new Blob([this.__bytes.slice(start, Math.max(start, end))], { type: type === undefined ? '' : type });
  });
  method(B, 'stream', function () { return makeStream(this.__bytes); });
  function File(parts, name, options) {
    if (!(this instanceof File)) throw typeError("construct 'File'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (arguments.length < 2) throw typeError("construct 'File'", "2 arguments required, but only " + arguments.length + " present.");
    Blob.call(this, parts, options);
    this.name = String(name);
    this.lastModified = options && options.lastModified !== undefined ? Number(options.lastModified) : Date.now();
  }
  File.prototype = Object.create(Blob.prototype);
  method(File.prototype, 'constructor', File);
  hide(g, 'Blob', Blob);
  hide(g, 'File', File);

  var blobUrls = {}, blobCounter = 0;
  URL.createObjectURL = function (blob) {
    if (!(blob instanceof Blob)) throw typeError("execute 'createObjectURL' on 'URL'", "Overload resolution failed.");
    var origin = location.origin === 'null' ? 'null' : location.origin;
    var url = 'blob:' + origin + '/' + (++blobCounter) + '-' + Date.now();
    blobUrls[url] = blob;
    return url;
  };
  URL.revokeObjectURL = function (url) { delete blobUrls[String(url)]; };

  // ---------------------------------------------------------------------------------------------------------------------------- FileReader
  function FileReader() {
    this.readyState = 0; this.result = null; this.error = null;
    this.onloadstart = this.onprogress = this.onload = this.onabort = this.onerror = this.onloadend = null;
  }
  FileReader.prototype = Object.create(EventTarget.prototype);
  method(FileReader.prototype, 'constructor', FileReader);
  FileReader.EMPTY = 0; FileReader.LOADING = 1; FileReader.DONE = 2;
  function readBlob(reader, blob, convert) {
    if (!(blob instanceof Blob)) throw typeError("execute 'read' on 'FileReader'", "parameter 1 is not of type 'Blob'.");
    if (reader.readyState === 1) throw new DOMException("Failed to execute 'read' on 'FileReader': The object is already busy reading Blobs.", 'InvalidStateError');
    reader.readyState = 1;
    reader.result = null;
    var fire = function (type) { reader.dispatchEvent(new ProgressEvent(type, { lengthComputable: true, loaded: blob.size, total: blob.size })); };
    fire('loadstart');
    setTimeout(function () {
      if (reader.readyState !== 1) return;
      fire('progress');
      reader.result = convert(blob);
      reader.readyState = 2;
      fire('load');
      fire('loadend');
    }, 0);
  }
  method(FileReader.prototype, 'readAsText', function (blob) { readBlob(this, blob, function (b) { return __utf8_clean(b.__bytes); }); });
  method(FileReader.prototype, 'readAsArrayBuffer', function (blob) { readBlob(this, blob, function (b) { return __string_to_bytes(b.__bytes); }); });
  method(FileReader.prototype, 'readAsBinaryString', function (blob) { readBlob(this, blob, function (b) { return b.__bytes; }); });
  method(FileReader.prototype, 'readAsDataURL', function (blob) {
    readBlob(this, blob, function (b) { return 'data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(b.__bytes); });
  });
  method(FileReader.prototype, 'abort', function () {
    if (this.readyState !== 1) return;
    this.readyState = 2; this.result = null;
    this.dispatchEvent(new ProgressEvent('abort'));
    this.dispatchEvent(new ProgressEvent('loadend'));
  });
  hide(g, 'FileReader', FileReader);

  // ------------------------------------------------------------------------------------------------------------------------ AbortController
  function AbortSignal() { this.aborted = false; this.reason = undefined; this.onabort = null; }
  AbortSignal.prototype = Object.create(EventTarget.prototype);
  method(AbortSignal.prototype, 'constructor', AbortSignal);
  method(AbortSignal.prototype, 'throwIfAborted', function () { if (this.aborted) throw this.reason; });
  function signalAbort(signal, reason) {
    if (signal.aborted) return;
    signal.aborted = true;
    signal.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
    signal.dispatchEvent(new Event('abort'));
  }
  AbortSignal.abort = function (reason) { var s = new AbortSignal(); signalAbort(s, reason); return s; };
  AbortSignal.timeout = function (ms) {
    var s = new AbortSignal();
    setTimeout(function () { signalAbort(s, new DOMException('signal timed out', 'TimeoutError')); }, ms);
    return s;
  };
  AbortSignal.any = function (signals) {
    var s = new AbortSignal();
    for (var i = 0; i < signals.length; i++) {
      if (signals[i].aborted) { signalAbort(s, signals[i].reason); return s; }
    }
    signals.forEach(function (other) { other.addEventListener('abort', function () { signalAbort(s, other.reason); }); });
    return s;
  };
  function AbortController() {
    if (!(this instanceof AbortController)) throw typeError("construct 'AbortController'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    this.signal = new AbortSignal();
  }
  method(AbortController.prototype, 'abort', function (reason) { signalAbort(this.signal, reason); });
  hide(g, 'AbortSignal', AbortSignal);
  hide(g, 'AbortController', AbortController);

  // ---------------------------------------------------------------------------------------------------------------------- bodies and streams
  // A one-chunk stream over bytes: what `response.body` and `blob.stream()` give.
  function makeStream(bytes) {
    var stream = { locked: false };
    var chunk = bytes.length ? __string_to_bytes(bytes) : null;
    stream.getReader = function () {
      if (stream.locked) throw new TypeError("Failed to execute 'getReader' on 'ReadableStream': ReadableStream is locked");
      stream.locked = true;
      return {
        read: function () {
          if (chunk) { var c = chunk; chunk = null; return Promise.resolve({ done: false, value: c }); }
          return Promise.resolve({ done: true, value: undefined });
        },
        releaseLock: function () { stream.locked = false; },
        cancel: function () { chunk = null; return Promise.resolve(); },
        closed: Promise.resolve()
      };
    };
    stream.cancel = function () { chunk = null; return Promise.resolve(); };
    return stream;
  }

  function multipart(formData) {
    var boundary = '----TheoryCraftFormBoundary' + Math.floor(Math.random() * 1e9);
    var body = '';
    var entries = formData.entries();
    for (var i = 0; i < entries.length; i++) {
      var name = String(entries[i][0]).split('"').join('%22'), value = entries[i][1];
      body += '--' + boundary + '\r\n';
      if (value instanceof Blob) {
        var filename = value instanceof File ? value.name : 'blob';
        body += 'Content-Disposition: form-data; name="' + name + '"; filename="' + String(filename).split('"').join('%22') + '"\r\n';
        body += 'Content-Type: ' + (value.type || 'application/octet-stream') + '\r\n\r\n' + value.__bytes + '\r\n';
      } else {
        body += 'Content-Disposition: form-data; name="' + name + '"\r\n\r\n' + String(value) + '\r\n';
      }
    }
    body += '--' + boundary + '--\r\n';
    return { bytes: body, type: 'multipart/form-data; boundary=' + boundary };
  }

  // what a body (a string, Blob, bytes, URLSearchParams, FormData) is as bytes, and its content type
  function extractBody(init) {
    if (init === undefined || init === null) return { bytes: null, type: null };
    if (typeof init === 'string') return { bytes: init, type: 'text/plain;charset=UTF-8' };
    if (init instanceof URLSearchParams) return { bytes: init.toString(), type: 'application/x-www-form-urlencoded;charset=UTF-8' };
    if (init instanceof Blob) return { bytes: init.__bytes, type: init.type || null };
    if (typeof FormData !== 'undefined' && init instanceof FormData) return multipart(init);
    if (isObject(init) && typeof init.byteLength === 'number') return { bytes: __bytes_to_string(init), type: null };
    return { bytes: String(init), type: 'text/plain;charset=UTF-8' };
  }

  function parseFormBody(bytes, contentType) {
    var fd = new FormData();
    var type = lower(contentType || '');
    if (type.indexOf('application/x-www-form-urlencoded') === 0) {
      var list = [];
      parseQuery(list, bytes);
      for (var i = 0; i < list.length; i++) fd.append(list[i][0], list[i][1]);
      return fd;
    }
    var at = type.indexOf('boundary=');
    if (type.indexOf('multipart/form-data') === 0 && at >= 0) {
      var boundary = (contentType.slice(contentType.toLowerCase().indexOf('boundary=') + 9).split(';')[0]).trim();
      if (boundary.charAt(0) === '"') boundary = boundary.slice(1, boundary.length - 1);
      var parts = bytes.split('--' + boundary);
      for (var p = 1; p < parts.length; p++) {
        var part = parts[p];
        if (part.slice(0, 2) === '--') break;
        var split = part.indexOf('\r\n\r\n');
        if (split < 0) continue;
        var head = part.slice(0, split), data = part.slice(split + 4);
        if (data.slice(data.length - 2) === '\r\n') data = data.slice(0, data.length - 2);
        var nameAt = head.indexOf('name="');
        if (nameAt < 0) continue;
        var name = head.slice(nameAt + 6).split('"')[0];
        var fileAt = head.indexOf('filename="');
        if (fileAt >= 0) {
          var typeAt = head.toLowerCase().indexOf('content-type:');
          var ctype = typeAt >= 0 ? head.slice(typeAt + 13).split('\r\n')[0].trim() : '';
          fd.append(name, new File([data], head.slice(fileAt + 10).split('"')[0], { type: ctype }));
        } else {
          fd.append(name, __utf8_clean(data));
        }
      }
      return fd;
    }
    throw new TypeError("Failed to fetch: could not parse the body as form data");
  }

  // the body methods Request and Response share
  function installBody(proto) {
    var consume = function (self) {
      if (self.bodyUsed) return Promise.reject(new TypeError("Failed to execute 'body' on '" + (self instanceof Request ? 'Request' : 'Response') + "': body stream already read"));
      self.bodyUsed = true;
      return Promise.resolve(self.__bytes === null ? '' : self.__bytes);
    };
    method(proto, 'text', function () { return consume(this).then(__utf8_clean); });
    method(proto, 'json', function () { return consume(this).then(function (b) { return JSON.parse(__utf8_clean(b)); }); });
    method(proto, 'arrayBuffer', function () { return consume(this).then(__string_to_bytes); });
    method(proto, 'bytes', function () { return consume(this).then(__string_to_bytes); });
    method(proto, 'blob', function () {
      var type = this.headers.get('content-type') || '';
      return consume(this).then(function (b) { return new Blob([b], { type: type }); });
    });
    method(proto, 'formData', function () {
      var type = this.headers.get('content-type');
      return consume(this).then(function (b) { return parseFormBody(b, type); });
    });
  }

  // -------------------------------------------------------------------------------------------------------------------------------- Response
  var NULL_BODY = [101, 204, 205, 304];
  function Response(body, init) {
    if (!(this instanceof Response)) throw typeError("construct 'Response'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    init = init || {};
    var status = init.status === undefined ? 200 : Number(init.status);
    if (!(status >= 200 && status <= 599)) throw new RangeError("Failed to construct 'Response': The status provided (" + init.status + ") is outside the range [200, 599].");
    this.type = 'default';
    this.url = '';
    this.redirected = false;
    this.status = status;
    this.ok = status >= 200 && status <= 299;
    this.statusText = init.statusText === undefined ? '' : String(init.statusText);
    this.headers = new Headers(init.headers);
    this.bodyUsed = false;
    var b = extractBody(body);
    if (b.bytes !== null && NULL_BODY.indexOf(status) >= 0) throw typeError("construct 'Response'", "Response with null body status cannot have body");
    hide(this, '__bytes', b.bytes);
    if (b.type && !this.headers.has('content-type')) this.headers.set('content-type', b.type);
    this.body = b.bytes === null ? null : makeStream(b.bytes);
  }
  installBody(Response.prototype);
  method(Response.prototype, 'clone', function () {
    if (this.bodyUsed) throw new TypeError("Failed to execute 'clone' on 'Response': Response body is already used");
    var copy = Object.create(Response.prototype);
    copy.type = this.type; copy.url = this.url; copy.redirected = this.redirected; copy.status = this.status; copy.ok = this.ok;
    copy.statusText = this.statusText; copy.headers = new Headers(this.headers); copy.bodyUsed = false;
    hide(copy, '__bytes', this.__bytes);
    copy.body = this.__bytes === null ? null : makeStream(this.__bytes);
    return copy;
  });
  Response.error = function () {
    var r = Object.create(Response.prototype);
    r.type = 'error'; r.url = ''; r.redirected = false; r.status = 0; r.ok = false; r.statusText = ''; r.headers = new Headers(); r.bodyUsed = false;
    hide(r, '__bytes', null); r.body = null;
    return r;
  };
  Response.redirect = function (url, status) {
    status = status === undefined ? 302 : status;
    if ([301, 302, 303, 307, 308].indexOf(status) < 0) throw new RangeError("Failed to execute 'redirect' on 'Response': Invalid status code");
    var resolved = __url_resolve(String(url), location.href);
    if (resolved === null) throw typeError("execute 'redirect' on 'Response'", "Invalid URL");
    var r = new Response(null, { status: 200 });
    r.status = status; r.ok = false;
    r.headers.set('location', resolved);
    r.headers.__immutable = true;
    return r;
  };
  Response.json = function (data, init) {
    var text = JSON.stringify(data);
    if (text === undefined) throw new TypeError("Failed to execute 'json' on 'Response': The data is not JSON serializable");
    var r = new Response(text, init);
    if (!(init && init.headers && new Headers(init.headers).has('content-type'))) r.headers.set('content-type', 'application/json');
    return r;
  };
  // a response made from what the network answered
  function makeResponse(res, request) {
    var r = Object.create(Response.prototype);
    var cross = false;
    if (res.opaque) {
      r.type = 'opaque'; r.status = 0; r.statusText = ''; r.url = ''; r.headers = new Headers(); hide(r, '__bytes', null);
      r.ok = false; r.redirected = false; r.bodyUsed = false; r.body = null;
      return r;
    }
    var bytes = __bytes_to_string(res.body);
    r.type = request && request.__cross ? 'cors' : 'basic';
    r.url = res.url; r.redirected = res.redirected; r.status = res.status; r.ok = res.status >= 200 && res.status <= 299;
    r.statusText = res.statusText; r.headers = Headers.__fromBlock(res.headers); r.headers.__immutable = true; r.bodyUsed = false;
    hide(r, '__bytes', bytes);
    r.body = res.status === 204 || res.status === 205 || res.status === 304 ? null : makeStream(bytes);
    return r;
  }
  hide(g, 'Response', Response);

  // --------------------------------------------------------------------------------------------------------------------------------- Request
  var SIMPLE_METHODS = ['DELETE', 'GET', 'HEAD', 'OPTIONS', 'POST', 'PUT'];
  function Request(input, init) {
    if (!(this instanceof Request)) throw typeError("construct 'Request'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (arguments.length < 1) throw typeError("construct 'Request'", "1 argument required, but only 0 present.");
    init = init || {};
    var src = input instanceof Request ? input : null;
    var url;
    if (src) url = src.url;
    else {
      url = __url_resolve(String(input), location.href);
      if (url === null) throw typeError("construct 'Request'", "Failed to parse URL from " + input);
      var parts = new URL(url);
      if (parts.username || parts.password) throw typeError("construct 'Request'", "Request cannot be constructed from a URL that includes credentials: " + input);
    }
    var m = init.method !== undefined ? String(init.method) : (src ? src.method : 'GET');
    if (!validName(m)) throw typeError("construct 'Request'", "'" + m + "' is not a valid HTTP method.");
    var upper = m.toUpperCase();
    if (upper === 'CONNECT' || upper === 'TRACE' || upper === 'TRACK') throw typeError("construct 'Request'", "'" + m + "' HTTP method is unsupported.");
    if (SIMPLE_METHODS.indexOf(upper) >= 0) m = upper;
    this.method = m;
    this.url = url;
    this.headers = new Headers(init.headers !== undefined ? init.headers : (src ? src.headers : undefined));
    this.mode = init.mode !== undefined ? init.mode : (src ? src.mode : 'cors');
    this.credentials = init.credentials !== undefined ? init.credentials : (src ? src.credentials : 'same-origin');
    this.cache = init.cache !== undefined ? init.cache : (src ? src.cache : 'default');
    this.redirect = init.redirect !== undefined ? init.redirect : (src ? src.redirect : 'follow');
    this.referrer = init.referrer !== undefined ? init.referrer : (src ? src.referrer : 'about:client');
    this.referrerPolicy = init.referrerPolicy !== undefined ? init.referrerPolicy : (src ? src.referrerPolicy : '');
    this.integrity = init.integrity !== undefined ? init.integrity : (src ? src.integrity : '');
    this.keepalive = !!(init.keepalive !== undefined ? init.keepalive : (src && src.keepalive));
    this.destination = '';
    this.duplex = 'half';
    this.bodyUsed = false;
    if (['cors', 'no-cors', 'same-origin', 'navigate'].indexOf(this.mode) < 0) throw typeError("construct 'Request'", "The provided value '" + this.mode + "' is not a valid enum value of type RequestMode.");
    if (['omit', 'same-origin', 'include'].indexOf(this.credentials) < 0) throw typeError("construct 'Request'", "The provided value '" + this.credentials + "' is not a valid enum value of type RequestCredentials.");
    if (['follow', 'error', 'manual'].indexOf(this.redirect) < 0) throw typeError("construct 'Request'", "The provided value '" + this.redirect + "' is not a valid enum value of type RequestRedirect.");
    var signal = init.signal !== undefined ? init.signal : (src ? src.signal : null);
    var own = new AbortSignal();
    if (signal) {
      if (signal.aborted) signalAbort(own, signal.reason);
      else signal.addEventListener('abort', function () { signalAbort(own, signal.reason); });
    }
    this.signal = own;
    var b;
    if (init.body !== undefined && init.body !== null) {
      if (m === 'GET' || m === 'HEAD') throw typeError("construct 'Request'", "Request with GET/HEAD method cannot have body.");
      b = extractBody(init.body);
      if (b.type && !this.headers.has('content-type')) this.headers.set('content-type', b.type);
    } else if (src && init.body === undefined) {
      if (src.bodyUsed) throw typeError("construct 'Request'", "Cannot construct a Request with a Request object that has already been used.");
      b = { bytes: src.__bytes };
      if (src.__bytes !== null) src.bodyUsed = src.bodyUsed;
    } else {
      b = { bytes: null };
    }
    hide(this, '__bytes', b.bytes);
  }
  installBody(Request.prototype);
  method(Request.prototype, 'clone', function () {
    if (this.bodyUsed) throw new TypeError("Failed to execute 'clone' on 'Request': Request body is already used");
    return new Request(this);
  });
  hide(g, 'Request', Request);

  // ----------------------------------------------------------------------------------------------------------------------------------- fetch
  function networkError(res) {
    var message = 'Failed to fetch';
    if (res && res.error && res.error !== 'Failed to fetch') message += ' (' + res.error + ')';
    return new TypeError(message);
  }
  function fetch(input, init) {
    return new Promise(function (resolve, reject) {
      var request;
      try { request = new Request(input, init); } catch (e) { reject(e); return; }
      if (request.signal.aborted) { reject(request.signal.reason); return; }
      var url = request.url;
      if (url.slice(0, 5) === 'blob:') {
        var blob = blobUrls[url];
        if (!blob) { reject(new TypeError('Failed to fetch (blob URL not found)')); return; }
        var r = new Response(blob, { status: 200, headers: { 'content-type': blob.type || 'application/octet-stream' } });
        r.url = url;
        resolve(r);
        return;
      }
      var finished = false, timer = 0;
      var onabort = function () {
        if (finished) return;
        finished = true;
        if (timer) clearTimeout(timer);
        reject(request.signal.reason);
      };
      request.signal.addEventListener('abort', onabort);
      var bytes = request.__bytes;
      request.bodyUsed = bytes !== null;
      timer = __net_send({
        method: request.method, url: url, headers: request.headers.__block(), body: bytes, mode: request.mode,
        credentials: request.credentials, redirect: request.redirect
      }, function (res) {
        if (finished) return;
        finished = true;
        request.signal.removeEventListener('abort', onabort);
        if (!res.ok) { reject(networkError(res)); return; }
        var loc = new URL(res.opaque ? url : (res.url || url));
        request.__cross = !res.opaque && loc.origin !== location.origin;
        if (res.redirected && request.redirect === 'manual') { /* (redirects are only reported as such) */ }
        resolve(makeResponse(res, request));
      });
    });
  }
  hide(g, 'fetch', fetch);

  // ---------------------------------------------------------------------------------------------------------------------- XMLHttpRequest
  var FORBIDDEN_XHR_HEADERS = ['accept-charset', 'accept-encoding', 'access-control-request-headers', 'access-control-request-method', 'connection', 'content-length',
    'cookie', 'cookie2', 'date', 'dnt', 'expect', 'host', 'keep-alive', 'origin', 'referer', 'set-cookie', 'te', 'trailer', 'transfer-encoding', 'upgrade', 'via'];
  function XMLHttpRequestUpload() {}
  XMLHttpRequestUpload.prototype = Object.create(EventTarget.prototype);
  function XMLHttpRequest() {
    if (!(this instanceof XMLHttpRequest)) throw typeError("construct 'XMLHttpRequest'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    this.readyState = 0;
    this.status = 0;
    this.statusText = '';
    this.response = '';
    this.responseText = '';
    this.responseURL = '';
    this.responseXML = null;
    this.responseType = '';
    this.timeout = 0;
    this.withCredentials = false;
    this.upload = new XMLHttpRequestUpload();
    this.onreadystatechange = this.onloadstart = this.onprogress = this.onload = this.onerror = this.onabort = this.ontimeout = this.onloadend = null;
    hide(this, '__x', { method: 'GET', url: '', async: true, headers: new Headers(), sent: false, done: false, timer: 0, mime: null, responseHeaders: null, generation: 0 });
  }
  XMLHttpRequest.prototype = Object.create(EventTarget.prototype);
  method(XMLHttpRequest.prototype, 'constructor', XMLHttpRequest);
  var states = { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 };
  Object.keys(states).forEach(function (k) {
    XMLHttpRequest[k] = states[k];
    Object.defineProperty(XMLHttpRequest.prototype, k, { value: states[k] });
  });
  var X = XMLHttpRequest.prototype;
  function xhrFire(xhr, type, loaded, total) {
    var event = type === 'readystatechange' ? new Event(type) : new ProgressEvent(type, { lengthComputable: total > 0, loaded: loaded || 0, total: total || 0 });
    xhr.dispatchEvent(event);
  }
  function setState(xhr, state) {
    xhr.readyState = state;
    xhrFire(xhr, 'readystatechange');
  }
  method(X, 'open', function (m, url, async, user, password) {
    if (arguments.length < 2) throw typeError("execute 'open' on 'XMLHttpRequest'", "2 arguments required, but only " + arguments.length + " present.");
    m = String(m);
    if (!validName(m)) throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': '" + m + "' is not a valid HTTP method.", 'SyntaxError');
    var upper = m.toUpperCase();
    if (upper === 'CONNECT' || upper === 'TRACE' || upper === 'TRACK') throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': '" + m + "' HTTP method is unsupported.", 'SecurityError');
    var resolved = __url_resolve(String(url), location.href);
    if (resolved === null) throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': Invalid URL", 'SyntaxError');
    var x = this.__x;
    if (x.timer) clearTimeout(x.timer);
    x.generation++;
    x.method = SIMPLE_METHODS.indexOf(upper) >= 0 ? upper : m;
    x.url = resolved;
    x.async = async === undefined ? true : !!async;
    x.headers = new Headers();
    x.sent = false; x.done = false; x.timer = 0; x.responseHeaders = null; x.mime = null;
    if (!x.async && (this.timeout !== 0 || this.responseType !== '')) throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': Synchronous requests must not set a timeout or responseType.", 'InvalidAccessError');
    this.status = 0; this.statusText = ''; this.response = ''; this.responseText = ''; this.responseURL = ''; this.responseXML = null;
    if (this.readyState !== 1) setState(this, 1); else this.readyState = 1;
  });
  method(X, 'setRequestHeader', function (name, value) {
    var x = this.__x;
    if (this.readyState !== 1 || x.sent) throw new DOMException("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
    name = String(name);
    if (!validName(name)) throw new DOMException("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': '" + name + "' is not a valid HTTP header field name.", 'SyntaxError');
    var v = cleanValue(value);
    if (v === null) throw new DOMException("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': '" + value + "' is not a valid HTTP header field value.", 'SyntaxError');
    var lowered = lower(name);
    if (FORBIDDEN_XHR_HEADERS.indexOf(lowered) >= 0 || lowered.indexOf('proxy-') === 0 || lowered.indexOf('sec-') === 0) return;
    x.headers.append(lowered, v);   // (a header set twice is combined)
  });
  method(X, 'overrideMimeType', function (mime) {
    if (this.readyState === 3 || this.readyState === 4) throw new DOMException("Failed to execute 'overrideMimeType' on 'XMLHttpRequest': MimeType cannot be overridden when the state is LOADING or DONE.", 'InvalidStateError');
    this.__x.mime = String(mime);
  });
  function xhrFinishError(xhr, kind) {
    var x = xhr.__x;
    x.done = true;
    xhr.status = 0; xhr.statusText = ''; xhr.response = xhr.responseType === '' || xhr.responseType === 'text' ? '' : null; xhr.responseText = '';
    setState(xhr, 4);
    xhrFire(xhr, kind);
    xhrFire(xhr, 'loadend');
  }
  function xhrAnswer(xhr, res, generation) {
    var x = xhr.__x;
    if (x.generation !== generation || x.done) return;
    x.timer = 0;
    if (!res.ok) {
      if (res.error && res.error.indexOf('timed out') >= 0 && xhr.timeout > 0) xhrFinishError(xhr, 'timeout');
      else xhrFinishError(xhr, 'error');
      return;
    }
    xhr.status = res.opaque ? 0 : res.status;
    xhr.statusText = res.opaque ? '' : res.statusText;
    xhr.responseURL = res.url;
    x.responseHeaders = Headers.__fromBlock(res.headers);
    setState(xhr, 2);
    if (x.generation !== generation) return;
    var bytes = __bytes_to_string(res.body);
    setState(xhr, 3);
    if (x.generation !== generation) return;
    xhrFire(xhr, 'progress', bytes.length, bytes.length);
    var type = xhr.responseType;
    var mime = x.mime || (x.responseHeaders.get('content-type') || '');
    if (type === '' || type === 'text') {
      xhr.responseText = __utf8_clean(bytes);
      xhr.response = xhr.responseText;
    } else if (type === 'json') {
      try { xhr.response = JSON.parse(__utf8_clean(bytes)); } catch (e) { xhr.response = null; }
    } else if (type === 'arraybuffer') {
      xhr.response = __string_to_bytes(bytes);
    } else if (type === 'blob') {
      xhr.response = new Blob([bytes], { type: mime });
    } else if (type === 'document') {
      var kind = lower(mime).indexOf('html') >= 0 ? 'text/html' : 'text/xml';
      try { xhr.response = new DOMParser().parseFromString(__utf8_clean(bytes), kind); xhr.responseXML = xhr.response; } catch (e) { xhr.response = null; }
    }
    x.done = true;
    setState(xhr, 4);
    if (x.generation !== generation) return;
    xhrFire(xhr, 'load', bytes.length, bytes.length);
    xhrFire(xhr, 'loadend', bytes.length, bytes.length);
  }
  method(X, 'send', function (body) {
    var x = this.__x;
    if (this.readyState !== 1 || x.sent) throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
    var bytes = null;
    if (body !== undefined && body !== null && x.method !== 'GET' && x.method !== 'HEAD') {
      var b = extractBody(body);
      if (isObject(body) && body.nodeType === 9) b = { bytes: body.documentElement ? body.documentElement.outerHTML : '', type: 'application/xml;charset=UTF-8' };
      bytes = b.bytes;
      if (b.type && !x.headers.has('content-type')) x.headers.set('content-type', b.type);
    }
    x.sent = true;
    var self = this, generation = x.generation;
    var options = {
      method: x.method, url: x.url, headers: x.headers.__block(), body: bytes, mode: 'cors',
      credentials: this.withCredentials ? 'include' : 'same-origin', redirect: 'follow', timeout: this.timeout
    };
    if (!x.async) {
      var res = __net_send_sync(options);
      xhrAnswer(this, res, generation);
      if (!res.ok) throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': Failed to load '" + x.url + "'.", 'NetworkError');
      return;
    }
    xhrFire(this, 'loadstart');
    if (x.generation !== generation) return;
    x.timer = __net_send(options, function (res) { xhrAnswer(self, res, generation); });
  });
  method(X, 'abort', function () {
    var x = this.__x;
    var active = (this.readyState === 1 && x.sent) || this.readyState === 2 || this.readyState === 3;
    if (x.timer) clearTimeout(x.timer);
    x.timer = 0;
    x.generation++;
    if (active) {
      x.done = true;
      x.sent = false;
      setState(this, 4);
      xhrFire(this, 'abort');
      xhrFire(this, 'loadend');
    }
    if (this.readyState === 4) { this.readyState = 0; x.done = false; }
    this.status = 0; this.statusText = ''; this.response = ''; this.responseText = '';
  });
  method(X, 'getResponseHeader', function (name) {
    var h = this.__x.responseHeaders;
    if (!h || this.readyState < 2) return null;
    return h.get(String(name));
  });
  method(X, 'getAllResponseHeaders', function () {
    var h = this.__x.responseHeaders;
    if (!h || this.readyState < 2) return '';
    var out = '';
    var entries = h.entries();
    for (var i = 0; i < entries.length; i++) out += entries[i][0] + ': ' + entries[i][1] + '\r\n';
    return out;
  });
  hide(g, 'XMLHttpRequest', XMLHttpRequest);
  hide(g, 'XMLHttpRequestUpload', XMLHttpRequestUpload);

  // ------------------------------------------------------------------------------------------ structured clone (a value as text and back)
  // Values are written as JSON in which everything that JSON cannot say is a tagged object: {"$":"u"} undefined, {"$":"n","v":"NaN"} a number, {"$":"a","e":[..]}
  // an array, {"$":"o","p":{..}} an object, {"$":"d","t":ms} a date, {"$":"b"|"f"|"u8"} a Blob, File or byte array, {"$":"x"} an error, {"$":"p","i":id} a message port
  // and {"$":"r","i":n} the n-th object written before (shared and circular references stay what they were).
  var PORT_TOKEN = {};
  var portRegistry = {};
  function dataCloneError(message) { return new DOMException(message, 'DataCloneError'); }
  function isBytes(v) { return isObject(v) && typeof v.byteLength === 'number' && v.BYTES_PER_ELEMENT === 1; }
  function serialize(value, transfer) {
    var seen = [], ids = [], transferred = [];
    if (transfer) {
      var list = Array.isArray(transfer) ? transfer : (isObject(transfer) && Array.isArray(transfer.transfer) ? transfer.transfer : []);
      for (var t = 0; t < list.length; t++) {
        if (list[t] instanceof MessagePort) {
          if (list[t].__detached) throw dataCloneError('Port is already detached.');
          if (transferred.indexOf(list[t]) < 0) { transferred.push(list[t]); ids.push(list[t].__id); }
        }
      }
    }
    function enc(v) {
      var type = typeof v;
      if (v === null || type === 'boolean' || type === 'string') return v;
      if (type === 'number') {
        if (v !== v) return { $: 'n', v: 'NaN' };
        if (v === Infinity) return { $: 'n', v: 'Infinity' };
        if (v === -Infinity) return { $: 'n', v: '-Infinity' };
        if (v === 0 && 1 / v < 0) return { $: 'n', v: '-0' };
        return v;
      }
      if (type === 'undefined') return { $: 'u' };
      if (type !== 'object') throw dataCloneError('A function could not be cloned.');
      var at = seen.indexOf(v);
      if (at >= 0) return { $: 'r', i: at };
      if (v instanceof MessagePort) {
        if (transferred.indexOf(v) < 0) throw dataCloneError('A MessagePort could not be cloned because it was not transferred.');
        seen.push(v);
        return { $: 'p', i: v.__id };
      }
      if (v instanceof Node || v === g || v instanceof Event || v instanceof Headers || v instanceof Request || v instanceof Response || v instanceof XMLHttpRequest ||
          v instanceof AbortSignal || v instanceof URL || v instanceof URLSearchParams) throw dataCloneError('#<' + (v.constructor && v.constructor.name ? v.constructor.name : 'Object') + '> could not be cloned.');
      seen.push(v);
      if (Array.isArray(v)) {
        var items = [];
        for (var i = 0; i < v.length; i++) items.push(enc(v[i]));
        return { $: 'a', e: items };
      }
      if (v instanceof Date) return { $: 'd', t: v.getTime() };
      if (v instanceof Blob) return { $: v instanceof File ? 'f' : 'b', b: btoa(v.__bytes), t: v.type, n: v.name, m: v.lastModified };
      if (isBytes(v)) return { $: 'u8', b: btoa(__bytes_to_string(v)) };
      if (v instanceof Error) return { $: 'x', n: v.name, m: v.message };
      var props = {};
      var keys = Object.keys(v);
      for (var k = 0; k < keys.length; k++) props[keys[k]] = enc(v[keys[k]]);
      return { $: 'o', p: props };
    }
    var text = JSON.stringify(enc(value));
    return { text: text, ids: ids, ports: transferred };
  }
  function deserialize(text, revived) {
    var objs = [];
    function dec(e) {
      if (e === null || typeof e !== 'object') return e;
      var out, i;
      switch (e.$) {
        case 'u': return undefined;
        case 'n': return e.v === 'NaN' ? NaN : e.v === 'Infinity' ? Infinity : e.v === '-Infinity' ? -Infinity : -0;
        case 'r': return objs[e.i];
        case 'a':
          out = []; objs.push(out);
          for (i = 0; i < e.e.length; i++) out.push(dec(e.e[i]));
          return out;
        case 'o':
          out = {}; objs.push(out);
          var keys = Object.keys(e.p);
          for (i = 0; i < keys.length; i++) out[keys[i]] = dec(e.p[keys[i]]);
          return out;
        case 'd': out = new Date(e.t); objs.push(out); return out;
        case 'b': out = new Blob([atob(e.b)], { type: e.t }); objs.push(out); return out;
        case 'f': out = new File([atob(e.b)], e.n, { type: e.t, lastModified: e.m }); objs.push(out); return out;
        case 'u8': out = __string_to_bytes(atob(e.b)); objs.push(out); return out;
        case 'x':
          out = e.n === 'TypeError' ? new TypeError(e.m) : e.n === 'RangeError' ? new RangeError(e.m) : e.n === 'SyntaxError' ? new SyntaxError(e.m) : e.n === 'ReferenceError' ? new ReferenceError(e.m) : new Error(e.m);
          objs.push(out); return out;
        case 'p':
          out = makePort(e.i); objs.push(out);
          if (revived) revived.push(out);
          return out;
      }
      return undefined;
    }
    return dec(JSON.parse(text));
  }
  hide(g, '__serialize', function (value) { return serialize(value, null).text; });
  hide(g, '__deserialize', function (text) { return deserialize(text, null); });
  hide(g, 'structuredClone', function (value, options) {
    if (arguments.length < 1) throw typeError("execute 'structuredClone' on 'Window'", "1 argument required, but only 0 present.");
    var s = serialize(value, options);
    for (var i = 0; i < s.ports.length; i++) s.ports[i].__detached = true;
    return deserialize(s.text, null);
  });

  // ----------------------------------------------------------------------------------------------------------------------- message ports
  function MessagePort(token, id) {
    if (token !== PORT_TOKEN) throw new TypeError('Illegal constructor');
    hide(this, '__id', id);
    hide(this, '__peer', id % 2 === 1 ? id + 1 : id - 1);
    hide(this, '__started', false);
    hide(this, '__detached', false);
    hide(this, '__closed', false);
    hide(this, '__queue', []);
    this.onmessage = null;
    this.onmessageerror = null;
  }
  MessagePort.prototype = Object.create(EventTarget.prototype);
  method(MessagePort.prototype, 'constructor', MessagePort);
  function makePort(id) {
    var old = portRegistry[id];
    if (old) old.__detached = true;
    var port = new MessagePort(PORT_TOKEN, id);
    portRegistry[id] = port;
    return port;
  }
  function flushPort(port) {
    while (port.__queue.length && (port.__started || typeof port.onmessage === 'function') && !port.__closed) {
      var item = port.__queue.shift();
      port.dispatchEvent(new MessageEvent('message', { data: item.data, ports: item.ports, origin: '', lastEventId: '', source: null }));
    }
  }
  method(MessagePort.prototype, 'postMessage', function (message, transfer) {
    if (this.__detached || this.__closed) return;
    var s = serialize(message, transfer);
    for (var i = 0; i < s.ports.length; i++) if (s.ports[i] === this) throw dataCloneError('Transfer of the port itself is not possible.');
    for (var j = 0; j < s.ports.length; j++) s.ports[j].__detached = true;
    __port_post(this.__peer, s.text, s.ids.join(','));
  });
  method(MessagePort.prototype, 'start', function () { this.__started = true; flushPort(this); });
  method(MessagePort.prototype, 'close', function () { this.__closed = true; this.__queue.length = 0; });
  method(MessagePort.prototype, '__receive', function (data, ports) {
    this.__queue.push({ data: data, ports: ports });
    flushPort(this);
  });
  function MessageChannel() {
    if (!(this instanceof MessageChannel)) throw typeError("construct 'MessageChannel'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    var first = __port_pair();
    this.port1 = makePort(first);
    this.port2 = makePort(first + 1);
  }
  hide(g, 'MessageChannel', MessageChannel);
  hide(g, 'MessagePort', MessagePort);
  function parseIds(csv) {
    var out = [];
    if (csv) { var parts = csv.split(','); for (var i = 0; i < parts.length; i++) out.push(Number(parts[i])); }
    return out;
  }
  // the ports that came with a message, in the order they were sent (the ones the data does not mention too)
  function arrivedPorts(ids, found) {
    var out = [];
    for (var i = 0; i < ids.length; i++) {
      var port = null;
      for (var k = 0; k < found.length; k++) if (found[k].__id === ids[i]) port = found[k];
      out.push(port || makePort(ids[i]));
    }
    return out;
  }
  hide(g, '__port_deliver', function (id, text, idsCsv) {
    var port = portRegistry[id];
    if (!port || port.__closed || port.__detached) return;
    var found = [], data;
    try { data = deserialize(text, found); } catch (e) { port.dispatchEvent(new MessageEvent('messageerror', { data: null })); return; }
    port.__receive(data, arrivedPorts(parseIds(idsCsv), found));
  });

  // -------------------------------------------------------------------------------------------------------------- window.postMessage
  var windowId = __window_id();
  var proxies = {};
  function postMessageTo(targetId, message, second, third) {
    var targetOrigin = '/', transfer = null;
    if (Array.isArray(second)) transfer = second;
    else if (isObject(second)) { if (second.targetOrigin !== undefined) targetOrigin = String(second.targetOrigin); transfer = second.transfer; }
    else if (second !== undefined) { targetOrigin = String(second); transfer = third; }
    if (targetOrigin !== '*' && targetOrigin !== '/') {
      var parsed;
      try { parsed = new URL(targetOrigin); } catch (e) { throw new DOMException("Failed to execute 'postMessage' on 'Window': Invalid target origin '" + targetOrigin + "' in a call to 'postMessage'.", 'SyntaxError'); }
      targetOrigin = parsed.origin;
    }
    var s = serialize(message, transfer);
    for (var i = 0; i < s.ports.length; i++) s.ports[i].__detached = true;
    __post_message(targetId, targetOrigin, s.text, s.ids.join(','));
  }
  function windowProxy(id) {
    if (id === windowId) return g;
    if (proxies[id]) return proxies[id];
    var p = {};
    hide(p, '__window', id);
    p.closed = false;
    method(p, 'postMessage', function (message, second, third) {
      if (arguments.length < 1) throw typeError("execute 'postMessage' on 'Window'", "1 argument required, but only 0 present.");
      postMessageTo(id, message, second, third);
    });
    proxies[id] = p;
    return p;
  }
  hide(g, '__window_proxy', windowProxy);
  hide(g, 'postMessage', function (message, second, third) {
    if (arguments.length < 1) throw typeError("execute 'postMessage' on 'Window'", "1 argument required, but only 0 present.");
    postMessageTo(windowId, message, second, third);
  });
  if (__parent_window_id()) {   // a page in a frame: window.parent is the page around it, window.top the outermost
    g.parent = windowProxy(__parent_window_id());
    g.top = windowProxy(__top_window_id());
  }
  hide(g, '__fire_message', function (text, origin, sourceId, idsCsv) {
    var found = [], data;
    try { data = deserialize(text, found); } catch (e) { g.dispatchEvent(new MessageEvent('messageerror', { data: null, origin: origin })); return; }
    g.dispatchEvent(new MessageEvent('message', { data: data, origin: origin, lastEventId: '', source: windowProxy(sourceId), ports: arrivedPorts(parseIds(idsCsv), found) }));
  });

  // ------------------------------------------------------------------------------------------------------------------- BroadcastChannel
  var channels = {};
  function BroadcastChannel(name) {
    if (!(this instanceof BroadcastChannel)) throw typeError("construct 'BroadcastChannel'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (arguments.length < 1) throw typeError("construct 'BroadcastChannel'", "1 argument required, but only 0 present.");
    this.name = String(name);
    this.onmessage = null;
    this.onmessageerror = null;
    hide(this, '__closed', false);
    (channels[this.name] = channels[this.name] || []).push(this);
  }
  BroadcastChannel.prototype = Object.create(EventTarget.prototype);
  method(BroadcastChannel.prototype, 'constructor', BroadcastChannel);
  method(BroadcastChannel.prototype, 'postMessage', function (message) {
    if (this.__closed) throw new DOMException('Failed to execute \'postMessage\' on \'BroadcastChannel\': Channel is closed', 'InvalidStateError');
    var text = serialize(message, null).text;
    var self = this, name = this.name;
    var others = (channels[name] || []).filter(function (c) { return c !== self; });
    others.forEach(function (c) {
      setTimeout(function () {
        if (c.__closed) return;
        c.dispatchEvent(new MessageEvent('message', { data: deserialize(text, null), origin: location.origin, lastEventId: '', source: null, ports: [] }));
      }, 0);
    });
    __broadcast(name, text);
  });
  method(BroadcastChannel.prototype, 'close', function () {
    this.__closed = true;
    var list = channels[this.name] || [];
    var at = list.indexOf(this);
    if (at >= 0) list.splice(at, 1);
  });
  hide(g, 'BroadcastChannel', BroadcastChannel);
  hide(g, '__bc_incoming', function (name, text) {
    var list = (channels[name] || []).slice();
    for (var i = 0; i < list.length; i++) {
      if (list[i].__closed) continue;
      list[i].dispatchEvent(new MessageEvent('message', { data: deserialize(text, null), origin: location.origin, lastEventId: '', source: null, ports: [] }));
    }
  });

  // ----------------------------------------------------------------------------------------------- events the browser makes out of values
  hide(g, '__storage_event', function (session, key, oldValue, newValue, url) {
    g.dispatchEvent(new StorageEvent('storage', { key: key, oldValue: oldValue, newValue: newValue, url: url, storageArea: session ? sessionStorage : localStorage }));
  });
  hide(g, '__fire_popstate', function () { g.dispatchEvent(new PopStateEvent('popstate', { state: history.state })); });
  hide(g, '__fire_hashchange', function (oldURL, newURL) { g.dispatchEvent(new HashChangeEvent('hashchange', { oldURL: oldURL, newURL: newURL })); });
}(this));
)JSPRELUDE";

}  // namespace

const char* web_prelude() { return kPrelude; }

}  // namespace browser
