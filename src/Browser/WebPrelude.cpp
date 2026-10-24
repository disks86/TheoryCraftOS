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
  var __utf8_bytes_length = function (text) { return text.length; };   // (strings are bytes in this engine)

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
    var origin = location.origin === 'null' ? 'null' : location.origin;
    if (typeof g.MediaSource === 'function' && blob instanceof g.MediaSource) {   // a MediaSource: the address is for a <video> or <audio> to play it
      var address = 'blob:' + origin + '/' + (++blobCounter) + '-' + Date.now();
      if (!__mse_register_url(address, blob.__id)) throw new DOMException("Failed to execute 'createObjectURL' on 'URL': The MediaSource is not usable.", 'InvalidStateError');
      return address;
    }
    if (!(blob instanceof Blob)) throw typeError("execute 'createObjectURL' on 'URL'", "Overload resolution failed.");
    var url = 'blob:' + origin + '/' + (++blobCounter) + '-' + Date.now();
    blobUrls[url] = blob;
    if (blob.__bytes.length > 0) __blob_register(url, __string_to_bytes(blob.__bytes));   // (for <img>, <video> and <audio>)
    return url;
  };
  URL.revokeObjectURL = function (url) { url = String(url); delete blobUrls[url]; __mse_revoke_url(url); __blob_revoke(url); };

  // ------------------------------------------------------------------------------------------------------------ Media Source Extensions
  // MediaSource, SourceBuffer and SourceBufferList; the bytes, the frames and the ranges are the engine's (DomBindingsMse.cpp, Media/Mse.hpp). Appending is done
  // when appendBuffer() is called (so a full buffer throws at once, as the standard has it); the events follow as tasks of their own.
  var mseObjects = {};
  var mseError = function (message, name) { return new DOMException(message, name); };
  var accessor = function (proto, name, get, set) { Object.defineProperty(proto, name, { get: get, set: set, enumerable: true, configurable: true }); };
  var mseFire = function (target, type) { target.dispatchEvent(new Event(type)); };
  var mseLater = function (fn) { setTimeout(fn, 0); };

  function SourceBufferList() { throw new TypeError('Illegal constructor'); }
  SourceBufferList.prototype = Object.create(EventTarget.prototype);
  method(SourceBufferList.prototype, 'constructor', SourceBufferList);
  accessor(SourceBufferList.prototype, 'length', function () { return this.__items.length; });
  method(SourceBufferList.prototype, 'item', function (index) { return this.__items[index >>> 0] || null; });
  method(SourceBufferList.prototype, ITER, function () { return this.__items.slice(); });   // (the iterators of this engine are arrays)
  var newBufferList = function () {
    var list = Object.create(SourceBufferList.prototype);
    hide(list, '__items', []);
    list.onaddsourcebuffer = null;
    list.onremovesourcebuffer = null;
    return list;
  };
  var listAdd = function (list, buffer) { list.__items.push(buffer); list[list.__items.length - 1] = buffer; };
  var listRemove = function (list, buffer) {
    var at = list.__items.indexOf(buffer);
    if (at < 0) return false;
    list.__items.splice(at, 1);
    delete list[list.__items.length];
    for (var i = 0; i < list.__items.length; i++) list[i] = list.__items[i];
    return true;
  };

  function MediaSource() {
    if (!(this instanceof MediaSource)) throw typeError("construct 'MediaSource'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    var id = __mse_create();
    if (!id) throw mseError("Failed to construct 'MediaSource': There are too many MediaSource objects.", 'QuotaExceededError');
    hide(this, '__id', id);
    hide(this, '__buffers', newBufferList());
    hide(this, '__active', newBufferList());
    this.onsourceopen = null;
    this.onsourceended = null;
    this.onsourceclose = null;
    mseObjects[id] = this;
  }
  MediaSource.prototype = Object.create(EventTarget.prototype);
  method(MediaSource.prototype, 'constructor', MediaSource);
  MediaSource.isTypeSupported = function (type) {
    if (arguments.length < 1) throw typeError("execute 'isTypeSupported' on 'MediaSource'", "1 argument required, but only 0 present.");
    return __mse_supported(String(type));
  };
  MediaSource.canConstructInDedicatedWorker = false;
  accessor(MediaSource.prototype, 'readyState', function () { var s = __mse_state(this.__id); return s === 1 ? 'open' : s === 2 ? 'ended' : 'closed'; });
  accessor(MediaSource.prototype, 'sourceBuffers', function () { return this.__buffers; });
  accessor(MediaSource.prototype, 'activeSourceBuffers', function () { return this.__active; });
  var anyUpdating = function (ms) {
    for (var i = 0; i < ms.__buffers.__items.length; i++) if (ms.__buffers.__items[i].__updating) return true;
    return false;
  };
  accessor(MediaSource.prototype, 'duration', function () { return __mse_duration(this.__id); }, function (value) {
    value = Number(value);
    if (value !== value) throw typeError("set the 'duration' property on 'MediaSource'", "The provided double value is non-finite.");
    if (value < 0) throw typeError("set the 'duration' property on 'MediaSource'", "The value provided (" + value + ") is outside the range [0, Infinity].");
    if (__mse_state(this.__id) !== 1) throw mseError("Failed to set the 'duration' property on 'MediaSource': The MediaSource's readyState is not 'open'.", 'InvalidStateError');
    if (anyUpdating(this)) throw mseError("Failed to set the 'duration' property on 'MediaSource': The 'updating' attribute is true on one or more of this MediaSource's SourceBuffers.", 'InvalidStateError');
    if (!__mse_set_duration(this.__id, value)) throw mseError("Failed to set the 'duration' property on 'MediaSource': Setting duration to a value lower than the highest presentation timestamp of any buffered coded frames is not allowed.", 'InvalidStateError');
  });
  method(MediaSource.prototype, 'addSourceBuffer', function (type) {
    if (arguments.length < 1) throw typeError("execute 'addSourceBuffer' on 'MediaSource'", "1 argument required, but only 0 present.");
    type = String(type);
    if (type === '') throw typeError("execute 'addSourceBuffer' on 'MediaSource'", "The type provided ('') is empty.");
    if (!__mse_supported(type)) throw mseError("Failed to execute 'addSourceBuffer' on 'MediaSource': The type provided ('" + type + "') is unsupported.", 'NotSupportedError');
    var number = __mse_add_buffer(this.__id, type);
    if (number === -1) throw mseError("Failed to execute 'addSourceBuffer' on 'MediaSource': The MediaSource's readyState is not 'open'.", 'InvalidStateError');
    if (number < 0) throw mseError("Failed to execute 'addSourceBuffer' on 'MediaSource': This MediaSource has reached the limit of SourceBuffer objects it can handle. No additional SourceBuffer objects may be added.", 'QuotaExceededError');
    var buffer = Object.create(SourceBuffer.prototype);
    hide(buffer, '__id', number);
    hide(buffer, '__source', this);
    hide(buffer, '__updating', false);
    hide(buffer, '__job', 0);
    hide(buffer, '__tracks', null);
    buffer.onupdatestart = buffer.onupdate = buffer.onupdateend = buffer.onerror = buffer.onabort = null;
    listAdd(this.__buffers, buffer);
    listAdd(this.__active, buffer);
    var self = this;
    mseLater(function () { mseFire(self.__buffers, 'addsourcebuffer'); });
    return buffer;
  });
  method(MediaSource.prototype, 'removeSourceBuffer', function (buffer) {
    if (arguments.length < 1) throw typeError("execute 'removeSourceBuffer' on 'MediaSource'", "1 argument required, but only 0 present.");
    if (!(buffer instanceof SourceBuffer)) throw typeError("execute 'removeSourceBuffer' on 'MediaSource'", "parameter 1 is not of type 'SourceBuffer'.");
    if (this.__buffers.__items.indexOf(buffer) < 0) throw mseError("Failed to execute 'removeSourceBuffer' on 'MediaSource': The SourceBuffer provided is not contained in this MediaSource.", 'NotFoundError');
    dropBuffer(this, buffer, true);
  });
  // Takes a SourceBuffer out (removeSourceBuffer(), or the MediaSource closing).
  var dropBuffer = function (ms, buffer, native) {
    if (buffer.__updating) {
      buffer.__job++;
      buffer.__updating = false;
      mseFire(buffer, 'abort');
      mseFire(buffer, 'updateend');
    }
    if (native) __mse_remove_buffer(ms.__id, buffer.__id);
    listRemove(ms.__buffers, buffer);
    listRemove(ms.__active, buffer);
    buffer.__source = null;
    mseLater(function () { mseFire(ms.__buffers, 'removesourcebuffer'); });
  };
  var endNames = { network: 2, decode: 3 };
  method(MediaSource.prototype, 'endOfStream', function (error) {
    var code = 0;
    if (error !== undefined) {
      error = String(error);
      if (!(error in endNames)) throw typeError("execute 'endOfStream' on 'MediaSource'", "The provided value '" + error + "' is not a valid enum value of type EndOfStreamError.");
      code = endNames[error];
    }
    if (__mse_state(this.__id) !== 1) throw mseError("Failed to execute 'endOfStream' on 'MediaSource': The MediaSource's readyState is not 'open'.", 'InvalidStateError');
    if (anyUpdating(this)) throw mseError("Failed to execute 'endOfStream' on 'MediaSource': The 'updating' attribute is true on one or more of this MediaSource's SourceBuffers.", 'InvalidStateError');
    __mse_end_of_stream(this.__id, code);
    var self = this;
    mseLater(function () { mseFire(self, 'sourceended'); });
  });
  method(MediaSource.prototype, 'setLiveSeekableRange', function (start, end) {
    if (arguments.length < 2) throw typeError("execute 'setLiveSeekableRange' on 'MediaSource'", "2 arguments required, but only " + arguments.length + " present.");
    start = Number(start); end = Number(end);
    if (__mse_state(this.__id) !== 1) throw mseError("Failed to execute 'setLiveSeekableRange' on 'MediaSource': The MediaSource's readyState is not 'open'.", 'InvalidStateError');
    if (!(start >= 0) || !(end >= start)) throw typeError("execute 'setLiveSeekableRange' on 'MediaSource'", "The start must be a non-negative number and not greater than the end.");
    __mse_live_range(this.__id, start, end);
  });
  method(MediaSource.prototype, 'clearLiveSeekableRange', function () {
    if (__mse_state(this.__id) !== 1) throw mseError("Failed to execute 'clearLiveSeekableRange' on 'MediaSource': The MediaSource's readyState is not 'open'.", 'InvalidStateError');
    __mse_live_range(this.__id, NaN, 0);
  });
  // What the browser has to say about a MediaSource: an element took it (sourceopen), or let go (sourceclose).
  hide(g, '__mse_event', function (source, type) {
    var ms = mseObjects[source];
    if (!ms) return;
    if (type === 'sourceclose') {
      var items = ms.__buffers.__items.slice();
      for (var i = 0; i < items.length; i++) dropBuffer(ms, items[i], false);
    }
    mseFire(ms, type);
  });

  function SourceBuffer() { throw new TypeError('Illegal constructor'); }
  SourceBuffer.prototype = Object.create(EventTarget.prototype);
  method(SourceBuffer.prototype, 'constructor', SourceBuffer);
  var bufferName = function (what) { return "Failed to " + what + " on 'SourceBuffer': "; };
  var usable = function (buffer, what) {
    if (!buffer.__source) throw mseError(bufferName(what) + "This SourceBuffer has been removed from the parent media source.", 'InvalidStateError');
  };
  var idle = function (buffer, what) {
    usable(buffer, what);
    if (buffer.__updating) throw mseError(bufferName(what) + "This SourceBuffer is still processing an 'appendBuffer' or 'remove' operation.", 'InvalidStateError');
  };
  // An ended MediaSource that is appended to or changed opens again.
  var reopenSource = function (ms) {
    if (__mse_state(ms.__id) === 2) {
      __mse_reopen(ms.__id);
      mseLater(function () { mseFire(ms, 'sourceopen'); });
    }
  };
  accessor(SourceBuffer.prototype, 'updating', function () { return this.__updating; });
  accessor(SourceBuffer.prototype, 'buffered', function () {
    usable(this, "get the 'buffered' property");
    return __mse_buffered(this.__id);
  });
  accessor(SourceBuffer.prototype, 'mode', function () { return __mse_get(this.__id, 0) === 1 ? 'sequence' : 'segments'; }, function (value) {
    value = String(value);
    if (value !== 'segments' && value !== 'sequence') return;
    idle(this, "set the 'mode' property");
    reopenSource(this.__source);
    if (!__mse_set(this.__id, 0, value === 'sequence' ? 1 : 0)) throw typeError("set the 'mode' property on 'SourceBuffer'", "The mode '" + value + "' is not supported for the byte stream format of this SourceBuffer.");
  });
  accessor(SourceBuffer.prototype, 'timestampOffset', function () { return __mse_get(this.__id, 1); }, function (value) {
    value = Number(value);
    if (value !== value || value === Infinity || value === -Infinity) throw typeError("set the 'timestampOffset' property on 'SourceBuffer'", "The provided double value is non-finite.");
    idle(this, "set the 'timestampOffset' property");
    reopenSource(this.__source);
    __mse_set(this.__id, 1, value);
  });
  accessor(SourceBuffer.prototype, 'appendWindowStart', function () { return __mse_get(this.__id, 2); }, function (value) {
    value = Number(value);
    idle(this, "set the 'appendWindowStart' property");
    if (value !== value) throw typeError("set the 'appendWindowStart' property on 'SourceBuffer'", "The provided double value is non-finite.");
    if (value < 0 || value >= __mse_get(this.__id, 3)) throw typeError("set the 'appendWindowStart' property on 'SourceBuffer'", "The value provided (" + value + ") is outside the range [0, " + __mse_get(this.__id, 3) + ").");
    __mse_set(this.__id, 2, value);
  });
  accessor(SourceBuffer.prototype, 'appendWindowEnd', function () { return __mse_get(this.__id, 3); }, function (value) {
    value = Number(value);
    idle(this, "set the 'appendWindowEnd' property");
    if (value !== value) throw typeError("set the 'appendWindowEnd' property on 'SourceBuffer'", "The provided double value is non-finite.");
    if (value <= __mse_get(this.__id, 2)) throw typeError("set the 'appendWindowEnd' property on 'SourceBuffer'", "The value provided (" + value + ") is less than or equal to the start (" + __mse_get(this.__id, 2) + ").");
    __mse_set(this.__id, 3, value);
  });
  // The tracks the initialization segments announced, as lists to look through (the tracks themselves cannot be switched).
  var trackList = function (buffer, kind) {
    var tracks = __mse_tracks(buffer.__id), list = [];
    for (var i = 0; i + 1 < tracks.length; i += 2) {
      if (tracks[i] !== kind) continue;
      list.push(kind === 0 ? { id: String(tracks[i + 1]), kind: 'main', label: '', language: '', selected: true, sourceBuffer: buffer }
                           : { id: String(tracks[i + 1]), kind: 'main', label: '', language: '', enabled: true, sourceBuffer: buffer });
    }
    hide(list, 'getTrackById', function (id) { for (var k = 0; k < list.length; k++) if (list[k].id === String(id)) return list[k]; return null; });
    return list;
  };
  accessor(SourceBuffer.prototype, 'videoTracks', function () { return trackList(this, 0); });
  accessor(SourceBuffer.prototype, 'audioTracks', function () { return trackList(this, 1); });
  accessor(SourceBuffer.prototype, 'textTracks', function () { var list = []; hide(list, 'getTrackById', function () { return null; }); return list; });

  // Runs an operation as the standard's updating cycle: updating is true, updatestart, the work, then update (or error / abort) and updateend.
  var cycle = function (buffer, work) {
    buffer.__updating = true;
    var job = ++buffer.__job;
    mseLater(function () {
      if (buffer.__job !== job) return;
      mseFire(buffer, 'updatestart');
      mseLater(function () {
        if (buffer.__job !== job) return;
        var failed = work();
        if (!failed) mseEncrypted(buffer);
        buffer.__updating = false;
        mseFire(buffer, failed ? 'error' : 'update');
        mseFire(buffer, 'updateend');
        if (failed && buffer.__source) {
          var ms = buffer.__source;
          if (__mse_state(ms.__id) === 1) {   // (the append error algorithm: the stream is over, with a decode error)
            __mse_end_of_stream(ms.__id, 3);
            mseLater(function () { mseFire(ms, 'sourceended'); });
          }
        }
      });
    });
  };
  method(SourceBuffer.prototype, 'appendBuffer', function (data) {
    if (arguments.length < 1) throw typeError("execute 'appendBuffer' on 'SourceBuffer'", "1 argument required, but only 0 present.");
    if (!(data instanceof ArrayBuffer) && !ArrayBuffer.isView(data)) throw typeError("execute 'appendBuffer' on 'SourceBuffer'", "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
    idle(this, "execute 'appendBuffer'");
    var ms = this.__source;
    if (__mse_source_error(ms.__id) !== 0) throw mseError(bufferName("execute 'appendBuffer'") + "The HTMLMediaElement.error attribute is not null.", 'InvalidStateError');
    reopenSource(ms);
    var result = __mse_append(this.__id, data);
    if (result === 1 || result === 3) throw mseError(bufferName("execute 'appendBuffer'") + "The SourceBuffer is full, and cannot free space to append additional buffers.", 'QuotaExceededError');
    cycle(this, function () { return result === 2 || result === 4; });
  });
  method(SourceBuffer.prototype, 'remove', function (start, end) {
    if (arguments.length < 2) throw typeError("execute 'remove' on 'SourceBuffer'", arguments.length + " arguments required, but only " + arguments.length + " present.");
    start = Number(start); end = Number(end);
    idle(this, "execute 'remove'");
    var duration = __mse_duration(this.__source.__id);
    if (duration !== duration) throw typeError("execute 'remove' on 'SourceBuffer'", "The duration is NaN.");
    if (!(start >= 0) || start > duration) throw typeError("execute 'remove' on 'SourceBuffer'", "The start provided (" + start + ") is outside the range [0, " + duration + "].");
    if (!(end > start)) throw typeError("execute 'remove' on 'SourceBuffer'", "The end provided (" + end + ") is less than or equal to the start (" + start + ").");
    reopenSource(this.__source);
    var self = this;
    cycle(this, function () { __mse_remove(self.__id, start, end); return false; });
  });
  method(SourceBuffer.prototype, 'abort', function () {
    usable(this, "execute 'abort'");
    if (__mse_state(this.__source.__id) !== 1) throw mseError(bufferName("execute 'abort'") + "The parent media source's readyState is not 'open'.", 'InvalidStateError');
    if (this.__updating) {
      this.__job++;
      this.__updating = false;
      mseFire(this, 'abort');
      mseFire(this, 'updateend');
    }
    __mse_abort(this.__id);
  });
  method(SourceBuffer.prototype, 'changeType', function (type) {
    if (arguments.length < 1) throw typeError("execute 'changeType' on 'SourceBuffer'", "1 argument required, but only 0 present.");
    type = String(type);
    if (type === '') throw typeError("execute 'changeType' on 'SourceBuffer'", "The type provided ('') is empty.");
    if (!__mse_supported(type)) throw mseError(bufferName("execute 'changeType'") + "The type provided ('" + type + "') is unsupported.", 'NotSupportedError');
    idle(this, "execute 'changeType'");
    reopenSource(this.__source);
    if (__mse_change_type(this.__id, type) !== 0) throw mseError(bufferName("execute 'changeType'") + "The type provided ('" + type + "') is not compatible with this SourceBuffer's byte stream format.", 'NotSupportedError');
  });
  hide(g, 'MediaSource', MediaSource);
  hide(g, 'SourceBuffer', SourceBuffer);
  hide(g, 'SourceBufferList', SourceBufferList);

  // ------------------------------------------------------------------------------------------------------------ Encrypted Media Extensions
  // The one key system a machine without a licensed DRM module can offer is the one the standard defines itself, W3C Clear Key ("org.w3.clearkey"): the license
  // request is the list of key identifiers wanted, the license is a JSON Web Key Set holding the keys. This is that key system, whole: the objects of the API, the
  // sessions with their messages and key statuses (also persistent ones, kept in localStorage), and the keys, which go to the table the media decryption looks
  // them up in (DomBindingsMse.cpp, Media/Eme.hpp). Widevine, PlayReady and FairPlay need modules that are not here, and are refused as unsupported.
  var CLEARKEY = 'org.w3.clearkey';
  var W3C_SYSTEM_ID = [0x10, 0x77, 0xef, 0xec, 0xc0, 0xb2, 0x4d, 0x02, 0xac, 0xe3, 0x3c, 0x1e, 0x52, 0xe2, 0xfb, 0x4b];
  var B64URL = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
  var emeToken = 0;

  var emeBytesOf = function (data) {   // a copy of an ArrayBuffer's or a view's bytes, or null
    if (data instanceof ArrayBuffer) return new Uint8Array(new Uint8Array(data));
    if (ArrayBuffer.isView(data)) return new Uint8Array(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
    return null;
  };
  var b64uEncode = function (bytes) {
    var out = '';
    for (var i = 0; i < bytes.length; i += 3) {
      var v = (bytes[i] << 16) | ((i + 1 < bytes.length ? bytes[i + 1] : 0) << 8) | (i + 2 < bytes.length ? bytes[i + 2] : 0);
      out += B64URL.charAt((v >> 18) & 63) + B64URL.charAt((v >> 12) & 63);
      if (i + 1 < bytes.length) out += B64URL.charAt((v >> 6) & 63);
      if (i + 2 < bytes.length) out += B64URL.charAt(v & 63);
    }
    return out;
  };
  var b64uDecode = function (text) {   // base64url (also base64, padded or not) to bytes; null if it is none
    if (typeof text !== 'string') return null;
    text = text.replace(/=+$/, '').replace(/\+/g, '-').replace(/\//g, '_');
    if (text.length % 4 === 1) return null;
    var out = [], acc = 0, bits = 0;
    for (var i = 0; i < text.length; i++) {
      var v = B64URL.indexOf(text.charAt(i));
      if (v < 0) return null;
      acc = (acc << 6) | v;
      bits += 6;
      if (bits >= 8) { bits -= 8; out.push((acc >> bits) & 255); acc &= (1 << bits) - 1; }
    }
    return new Uint8Array(out);
  };
  var emeTextOf = function (bytes) { var s = ''; for (var i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]); return s; };
  var bytesOfText = function (text) { var out = new Uint8Array(text.length); for (var i = 0; i < text.length; i++) out[i] = text.charCodeAt(i) & 255; return out; };
  var sameBytes = function (a, b) {
    if (a.length !== b.length) return false;
    for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
  };
  var emeDom = function (message, name) { return new DOMException(message, name); };
  var storageAvailable = function () { try { g.localStorage.getItem('__eme_licenses'); return true; } catch (e) { return false; } };
  var savedLicenses = function () { try { return JSON.parse(g.localStorage.getItem('__eme_licenses') || '{}') || {}; } catch (e) { return {}; } };
  var saveLicenses = function (all) { try { g.localStorage.setItem('__eme_licenses', JSON.stringify(all)); } catch (e) {} };

  // The key identifiers an initData asks for: 'keyids' is JSON, 'cenc' is 'pssh' boxes (the W3C common system's version 1 box lists them), 'webm' is one identifier.
  var kidsOfInitData = function (type, data) {
    var kids = [], i;
    if (type === 'webm') { if (data.length === 16) kids.push(data); return kids; }
    if (type === 'keyids') {
      var json;
      try { json = JSON.parse(emeTextOf(data)); } catch (e) { return null; }
      if (!json || !Array.isArray(json.kids)) return null;
      for (i = 0; i < json.kids.length; i++) {
        var kid = b64uDecode(json.kids[i]);
        if (!kid || kid.length !== 16) return null;
        kids.push(kid);
      }
      return kids;
    }
    var at = 0;
    while (at < data.length) {
      if (at + 32 > data.length) return null;
      var size = ((data[at] << 24) | (data[at + 1] << 16) | (data[at + 2] << 8) | data[at + 3]) >>> 0;
      if (size < 32 || at + size > data.length || emeTextOf(data.subarray(at + 4, at + 8)) !== 'pssh') return null;
      if (data[at + 8] === 1 && sameBytes(data.subarray(at + 12, at + 28), W3C_SYSTEM_ID)) {
        var count = ((data[at + 28] << 24) | (data[at + 29] << 16) | (data[at + 30] << 8) | data[at + 31]) >>> 0;
        if (32 + count * 16 > size) return null;
        for (i = 0; i < count; i++) kids.push(data.slice(at + 32 + i * 16, at + 48 + i * 16));
      }
      at += size;
    }
    return kids;
  };

  function MediaKeyMessageEvent(type, init) {
    if (arguments.length < 2 || !init || typeof init.messageType !== 'string' || !(init.message instanceof ArrayBuffer || ArrayBuffer.isView(init.message)))
      throw typeError("construct 'MediaKeyMessageEvent'", "The message and messageType members are required.");
    var event = new Event(type, init);
    Object.setPrototypeOf(event, MediaKeyMessageEvent.prototype);
    Object.defineProperty(event, 'messageType', { value: init.messageType, enumerable: true });
    Object.defineProperty(event, 'message', { value: init.message instanceof ArrayBuffer ? init.message : emeBytesOf(init.message).buffer, enumerable: true });
    return event;
  }
  MediaKeyMessageEvent.prototype = Object.create(Event.prototype);
  method(MediaKeyMessageEvent.prototype, 'constructor', MediaKeyMessageEvent);
  function MediaEncryptedEvent(type, init) {
    var event = new Event(type, init);
    Object.setPrototypeOf(event, MediaEncryptedEvent.prototype);
    var data = init && init.initData ? (init.initData instanceof ArrayBuffer ? init.initData : emeBytesOf(init.initData).buffer) : null;
    Object.defineProperty(event, 'initDataType', { value: init && init.initDataType !== undefined ? String(init.initDataType) : '', enumerable: true });
    Object.defineProperty(event, 'initData', { value: data, enumerable: true });
    return event;
  }
  MediaEncryptedEvent.prototype = Object.create(Event.prototype);
  method(MediaEncryptedEvent.prototype, 'constructor', MediaEncryptedEvent);
  // "encrypted" at the media element: the init data a SourceBuffer's stream carries.
  var mseEncrypted = function (buffer) {
    var list = __mse_encrypted(buffer.__id);
    if (list.length === 0 || !buffer.__source) return;
    var element = __mse_element(buffer.__source.__id);
    if (!element) return;
    for (var i = 0; i + 1 < list.length; i += 2)
      element.dispatchEvent(new MediaEncryptedEvent('encrypted', { initDataType: list[i] === 2 ? 'webm' : list[i] === 1 ? 'keyids' : 'cenc', initData: list[i + 1].buffer }));
  };

  // MediaKeyStatusMap: the key identifiers of a session and the state of each.
  function MediaKeyStatusMap() { throw new TypeError('Illegal constructor'); }
  var newStatusMap = function () { var map = Object.create(MediaKeyStatusMap.prototype); hide(map, '__entries', []); return map; };
  var statusIndex = function (map, key) {
    var bytes = emeBytesOf(key);
    if (!bytes) throw typeError("execute on 'MediaKeyStatusMap'", "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
    for (var i = 0; i < map.__entries.length; i++) if (sameBytes(map.__entries[i][0], bytes)) return i;
    return -1;
  };
  accessor(MediaKeyStatusMap.prototype, 'size', function () { return this.__entries.length; });
  method(MediaKeyStatusMap.prototype, 'has', function (key) { return statusIndex(this, key) >= 0; });
  method(MediaKeyStatusMap.prototype, 'get', function (key) { var i = statusIndex(this, key); return i >= 0 ? this.__entries[i][1] : undefined; });
  method(MediaKeyStatusMap.prototype, 'entries', function () { return this.__entries.map(function (e) { return [e[0].slice().buffer, e[1]]; }); });
  method(MediaKeyStatusMap.prototype, 'keys', function () { return this.__entries.map(function (e) { return e[0].slice().buffer; }); });
  method(MediaKeyStatusMap.prototype, 'values', function () { return this.__entries.map(function (e) { return e[1]; }); });
  method(MediaKeyStatusMap.prototype, 'forEach', function (callback, thisArg) {
    if (typeof callback !== 'function') throw typeError("execute 'forEach' on 'MediaKeyStatusMap'", "The callback provided as parameter 1 is not a function.");
    for (var i = 0; i < this.__entries.length; i++) callback.call(thisArg, this.__entries[i][1], this.__entries[i][0].slice().buffer, this);
  });
  method(MediaKeyStatusMap.prototype, ITER, MediaKeyStatusMap.prototype.entries);

  function MediaKeySession() { throw new TypeError('Illegal constructor'); }
  MediaKeySession.prototype = Object.create(EventTarget.prototype);
  method(MediaKeySession.prototype, 'constructor', MediaKeySession);
  var newSession = function (keys, type) {
    var session = Object.create(MediaKeySession.prototype);
    hide(session, '__keys', keys);
    hide(session, '__type', type);
    hide(session, '__token', ++emeToken);
    hide(session, '__id', '');
    hide(session, '__state', 'new');   // new, active, releasing, closed
    hide(session, '__map', newStatusMap());
    var resolveClosed;
    hide(session, '__closed', new Promise(function (resolve) { resolveClosed = resolve; }));
    hide(session, '__resolveClosed', resolveClosed);
    session.onmessage = null;
    session.onkeystatuseschange = null;
    return session;
  };
  accessor(MediaKeySession.prototype, 'sessionId', function () { return this.__id; });
  accessor(MediaKeySession.prototype, 'expiration', function () { return NaN; });
  accessor(MediaKeySession.prototype, 'closed', function () { return this.__closed; });
  accessor(MediaKeySession.prototype, 'keyStatuses', function () { return this.__map; });
  var sessionMessage = function (session, messageType, text) {
    var message = bytesOfText(text).buffer;
    mseLater(function () { session.dispatchEvent(new MediaKeyMessageEvent('message', { messageType: messageType, message: message })); });
  };
  var statusChanged = function (session) { mseLater(function () { mseFire(session, 'keystatuseschange'); }); };
  // sets the status of key ids; true if anything changed
  var setStatus = function (session, kids, status) {
    var changed = false;
    kids.forEach(function (kid) {
      var at = statusIndex(session.__map, kid);
      if (at < 0) { session.__map.__entries.push([kid, status]); changed = true; }
      else if (session.__map.__entries[at][1] !== status) { session.__map.__entries[at][1] = status; changed = true; }
    });
    return changed;
  };
  var closeSession = function (session) {
    if (session.__state === 'closed') return;
    __eme_remove_session(session.__keys.__id, session.__token);
    session.__state = 'closed';
    session.__map.__entries.length = 0;
    var listed = session.__keys.__sessions.indexOf(session);
    if (listed >= 0) session.__keys.__sessions.splice(listed, 1);
    session.__resolveClosed('closed-by-application');
  };
  var kidList = function (kids) { return kids.map(function (k) { return '"' + b64uEncode(k) + '"'; }).join(','); };
  var rejectedWith = function (error) { return Promise.reject(error); };
  var callable = function (session) {
    if (session.__state === 'closed') return emeDom('The session is already closed.', 'InvalidStateError');
    if (session.__id === '') return emeDom('The session is not callable.', 'InvalidStateError');
    return null;
  };

  method(MediaKeySession.prototype, 'generateRequest', function (initDataType, initData) {
    var session = this;
    if (session.__state === 'closed') return rejectedWith(emeDom('The session is already closed.', 'InvalidStateError'));
    if (session.__id !== '') return rejectedWith(emeDom('The session is already initialized.', 'InvalidStateError'));
    initDataType = String(initDataType);
    var data = emeBytesOf(initData);
    if (!data) return rejectedWith(typeError("execute 'generateRequest' on 'MediaKeySession'", "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'."));
    if (initDataType === '' || data.length === 0) return rejectedWith(typeError("execute 'generateRequest' on 'MediaKeySession'", "The initData parameter is empty."));
    if (initDataType !== 'cenc' && initDataType !== 'keyids' && initDataType !== 'webm')
      return rejectedWith(emeDom("The initialization data type '" + initDataType + "' is not supported by the key system.", 'NotSupportedError'));
    var kids = kidsOfInitData(initDataType, data);
    if (kids === null) return rejectedWith(typeError("execute 'generateRequest' on 'MediaKeySession'", "The initialization data provided could not be parsed."));
    if (kids.length === 0) return rejectedWith(emeDom('The initialization data holds no key identifiers this key system can use.', 'NotSupportedError'));
    session.__id = Math.random().toString(36).slice(2, 10) + Date.now().toString(36);
    session.__state = 'active';
    session.__requested = kids;
    session.__keys.__sessions.push(session);
    sessionMessage(session, 'license-request', '{"kids":[' + kidList(kids) + '],"type":"' + session.__type + '"}');
    return Promise.resolve();
  });

  // The keys of a JSON Web Key Set, checked whole before any is used: [[kid, key], ...] or an error to reject with.
  var keysOfLicense = function (session, bytes) {
    var json;
    try { json = JSON.parse(emeTextOf(bytes)); } catch (e) { return typeError("execute 'update' on 'MediaKeySession'", "The response is not valid JSON."); }
    if (!json || !Array.isArray(json.keys) || json.keys.length === 0) return typeError("execute 'update' on 'MediaKeySession'", "The response is not a JSON Web Key Set with keys in it.");
    if (json.type !== undefined && json.type !== session.__type) return typeError("execute 'update' on 'MediaKeySession'", "The license type '" + json.type + "' is not the type of the session.");
    var out = [];
    for (var i = 0; i < json.keys.length; i++) {
      var jwk = json.keys[i];
      if (!jwk || jwk.kty !== 'oct') return typeError("execute 'update' on 'MediaKeySession'", "Every key has to be of type 'oct'.");
      var kid = b64uDecode(jwk.kid), key = b64uDecode(jwk.k);
      if (!kid || kid.length !== 16 || !key || key.length !== 16) return typeError("execute 'update' on 'MediaKeySession'", "A key and its identifier have to be 16 bytes.");
      out.push([kid, key]);
    }
    return out;
  };
  var rememberLicense = function (session, pairs) {
    if (session.__type !== 'persistent-license') return;
    var all = savedLicenses();
    var record = all[session.__id] || { keys: [] };
    pairs.forEach(function (p) {
      var kid = b64uEncode(p[0]);
      record.keys = record.keys.filter(function (k) { return k[0] !== kid; });
      record.keys.push([kid, b64uEncode(p[1])]);
    });
    all[session.__id] = record;
    saveLicenses(all);
  };
  method(MediaKeySession.prototype, 'update', function (response) {
    var session = this;
    var problem = callable(session);
    if (problem) return rejectedWith(problem);
    var bytes = emeBytesOf(response);
    if (!bytes) return rejectedWith(typeError("execute 'update' on 'MediaKeySession'", "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'."));
    if (bytes.length === 0) return rejectedWith(typeError("execute 'update' on 'MediaKeySession'", "The response parameter is empty."));
    if (session.__state === 'releasing') {   // the acknowledgement of a license release: the session is over
      try { JSON.parse(emeTextOf(bytes)); } catch (e) { return rejectedWith(typeError("execute 'update' on 'MediaKeySession'", "The response is not valid JSON.")); }
      closeSession(session);
      return Promise.resolve();
    }
    var pairs = keysOfLicense(session, bytes);
    if (!Array.isArray(pairs)) return rejectedWith(pairs);
    for (var i = 0; i < pairs.length; i++)
      if (!__eme_add_key(session.__keys.__id, session.__token, pairs[i][0], pairs[i][1])) return rejectedWith(emeDom('The key table is full.', 'QuotaExceededError'));
    if (setStatus(session, pairs.map(function (p) { return p[0]; }), 'usable')) statusChanged(session);
    rememberLicense(session, pairs);
    return Promise.resolve();
  });
  method(MediaKeySession.prototype, 'close', function () {
    var session = this;
    if (session.__state === 'closed') return Promise.resolve();
    if (session.__id === '' && session.__state === 'new') return rejectedWith(emeDom('The session is not callable.', 'InvalidStateError'));
    closeSession(session);
    return Promise.resolve();
  });
  method(MediaKeySession.prototype, 'remove', function () {
    var session = this;
    var problem = callable(session);
    if (problem) return rejectedWith(problem);
    var kids = session.__map.__entries.map(function (e) { return e[0]; });
    if (session.__type === 'persistent-license') {
      var all = savedLicenses();
      delete all[session.__id];
      saveLicenses(all);
      __eme_remove_session(session.__keys.__id, session.__token);
      setStatus(session, kids, 'released');
      session.__state = 'releasing';
      statusChanged(session);
      sessionMessage(session, 'license-release', '{"kids":[' + kidList(kids) + ']}');
      return Promise.resolve();
    }
    closeSession(session);
    return Promise.resolve();
  });
  method(MediaKeySession.prototype, 'load', function (sessionId) {
    var session = this;
    if (session.__type !== 'persistent-license') return rejectedWith(typeError("execute 'load' on 'MediaKeySession'", "The session is not a persistent one."));
    sessionId = String(sessionId);
    if (sessionId === '') return rejectedWith(typeError("execute 'load' on 'MediaKeySession'", "The session ID is empty."));
    if (session.__state === 'closed') return rejectedWith(emeDom('The session is already closed.', 'InvalidStateError'));
    if (session.__id !== '') return rejectedWith(emeDom('The session is already initialized.', 'InvalidStateError'));
    var record = savedLicenses()[sessionId];
    if (!record || !Array.isArray(record.keys)) return Promise.resolve(false);
    var kids = [];
    for (var i = 0; i < record.keys.length; i++) {
      var kid = b64uDecode(record.keys[i][0]), key = b64uDecode(record.keys[i][1]);
      if (!kid || !key || kid.length !== 16 || key.length !== 16) return Promise.resolve(false);
      __eme_add_key(session.__keys.__id, session.__token, kid, key);
      kids.push(kid);
    }
    session.__id = sessionId;
    session.__state = 'active';
    session.__keys.__sessions.push(session);
    if (setStatus(session, kids, 'usable')) statusChanged(session);
    return Promise.resolve(true);
  });

  function MediaKeys() { throw new TypeError('Illegal constructor'); }
  method(MediaKeys.prototype, 'createSession', function (sessionType) {
    sessionType = sessionType === undefined ? 'temporary' : String(sessionType);
    if (sessionType !== 'temporary' && sessionType !== 'persistent-license' && sessionType !== 'persistent-usage-record')
      throw typeError("execute 'createSession' on 'MediaKeys'", "The provided value '" + sessionType + "' is not a valid enum value of type MediaKeySessionType.");
    if (this.__sessionTypes.indexOf(sessionType) < 0) throw emeDom("The session type '" + sessionType + "' is not supported.", 'NotSupportedError');
    return newSession(this, sessionType);
  });
  method(MediaKeys.prototype, 'setServerCertificate', function (certificate) {
    if (!emeBytesOf(certificate)) return rejectedWith(typeError("execute 'setServerCertificate' on 'MediaKeys'", "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'."));
    return Promise.resolve(false);   // (Clear Key has no use for one)
  });
  method(MediaKeys.prototype, 'getStatusForPolicy', function (policy) {
    return Promise.resolve('usable');
  });

  function MediaKeySystemAccess() { throw new TypeError('Illegal constructor'); }
  method(MediaKeySystemAccess.prototype, 'getConfiguration', function () { return JSON.parse(JSON.stringify(this.__config)); });
  method(MediaKeySystemAccess.prototype, 'createMediaKeys', function () {
    var access = this;
    return new Promise(function (resolve, reject) {
      var id = __eme_create_keys();
      if (!id) { reject(emeDom('There are too many MediaKeys objects.', 'QuotaExceededError')); return; }
      var keys = Object.create(MediaKeys.prototype);
      hide(keys, '__id', id);
      hide(keys, '__sessions', []);
      hide(keys, '__sessionTypes', access.__config.sessionTypes.slice());
      hide(keys, '__element', null);
      resolve(keys);
    });
  });

  // The configuration of a request, cut down to what Clear Key does; null if nothing of it is possible.
  var capabilitiesOf = function (list) {
    var out = [];
    for (var i = 0; i < list.length; i++) {
      var cap = list[i];
      if (!cap || typeof cap.contentType !== 'string' || !__mse_supported(cap.contentType)) continue;
      if (cap.robustness !== undefined && cap.robustness !== '') continue;
      var scheme = cap.encryptionScheme === undefined ? null : cap.encryptionScheme;
      if (scheme !== null && scheme !== 'cenc' && scheme !== 'cbcs' && scheme !== 'cbcs-1-9') continue;
      out.push({ contentType: cap.contentType, robustness: '', encryptionScheme: scheme });
    }
    return out;
  };
  var negotiate = function (config) {
    if (!isObject(config)) return null;
    var result = { initDataTypes: [], audioCapabilities: [], videoCapabilities: [], distinctiveIdentifier: 'not-allowed', persistentState: 'not-allowed', sessionTypes: ['temporary'], label: config.label === undefined ? '' : String(config.label) };
    if (Array.isArray(config.initDataTypes) && config.initDataTypes.length > 0) {
      result.initDataTypes = config.initDataTypes.map(String).filter(function (t) { return t === 'cenc' || t === 'keyids' || t === 'webm'; });
      if (result.initDataTypes.length === 0) return null;
    }
    if (config.distinctiveIdentifier === 'required') return null;
    var wantsPersistent = false;
    if (config.sessionTypes !== undefined) {
      if (!Array.isArray(config.sessionTypes)) return null;
      for (var i = 0; i < config.sessionTypes.length; i++) {
        var type = String(config.sessionTypes[i]);
        if (type === 'persistent-license') wantsPersistent = true;
        else if (type !== 'temporary') return null;
      }
      result.sessionTypes = config.sessionTypes.map(String);
    }
    if (config.persistentState === 'required' || wantsPersistent) {
      if (config.persistentState === 'not-allowed' || !storageAvailable()) return null;
      result.persistentState = 'required';
    }
    if (Array.isArray(config.audioCapabilities) && config.audioCapabilities.length > 0) {
      result.audioCapabilities = capabilitiesOf(config.audioCapabilities);
      if (result.audioCapabilities.length === 0) return null;
    }
    if (Array.isArray(config.videoCapabilities) && config.videoCapabilities.length > 0) {
      result.videoCapabilities = capabilitiesOf(config.videoCapabilities);
      if (result.videoCapabilities.length === 0) return null;
    }
    return result;
  };
  var requestAccess = function (keySystem, configs) {
    if (arguments.length < 2) return rejectedWith(typeError("execute 'requestMediaKeySystemAccess' on 'Navigator'", arguments.length + " arguments required, but only " + arguments.length + " present."));
    keySystem = String(keySystem);
    if (!Array.isArray(configs)) return rejectedWith(typeError("execute 'requestMediaKeySystemAccess' on 'Navigator'", "The provided value cannot be converted to a sequence."));
    if (configs.length === 0) return rejectedWith(typeError("execute 'requestMediaKeySystemAccess' on 'Navigator'", "The supportedConfigurations parameter is empty."));
    if (keySystem !== CLEARKEY) return rejectedWith(emeDom('Unsupported keySystem or supportedConfigurations.', 'NotSupportedError'));
    for (var i = 0; i < configs.length; i++) {
      var accumulated = negotiate(configs[i]);
      if (!accumulated) continue;
      var access = Object.create(MediaKeySystemAccess.prototype);
      hide(access, '__config', accumulated);
      Object.defineProperty(access, 'keySystem', { value: CLEARKEY, enumerable: true });
      return Promise.resolve(access);
    }
    return rejectedWith(emeDom('Unsupported keySystem or supportedConfigurations.', 'NotSupportedError'));
  };
  if (g.navigator && !g.navigator.requestMediaKeySystemAccess) hide(g.navigator, 'requestMediaKeySystemAccess', requestAccess);

  // The media element: setMediaKeys() says which MediaKeys decrypts what it plays.
  if (g.HTMLMediaElement) {
    var MP = g.HTMLMediaElement.prototype;
    accessor(MP, 'mediaKeys', function () { return this.__mediaKeys || null; });
    method(MP, 'setMediaKeys', function (keys) {
      var element = this;
      return new Promise(function (resolve, reject) {
        if (keys !== null && !(keys instanceof MediaKeys)) { reject(typeError("execute 'setMediaKeys' on 'HTMLMediaElement'", "The provided value is not of type 'MediaKeys'.")); return; }
        if (keys && keys.__element && keys.__element !== element) { reject(emeDom('The MediaKeys object is in use by another media element.', 'QuotaExceededError')); return; }
        if (element.__mediaKeys) element.__mediaKeys.__element = null;
        Object.defineProperty(element, '__mediaKeys', { value: keys, writable: true, configurable: true });
        if (keys) keys.__element = element;
        __eme_set_keys(element, keys ? keys.__id : 0);
        resolve();
      });
    });
  }
  hide(g, 'MediaKeySystemAccess', MediaKeySystemAccess);
  hide(g, 'MediaKeys', MediaKeys);
  hide(g, 'MediaKeySession', MediaKeySession);
  hide(g, 'MediaKeyStatusMap', MediaKeyStatusMap);
  hide(g, 'MediaKeyMessageEvent', MediaKeyMessageEvent);
  hide(g, 'MediaEncryptedEvent', MediaEncryptedEvent);

  // navigator.mediaCapabilities: what players ask before choosing a rendition. What can be played is what a SourceBuffer takes.
  if (g.navigator && !g.navigator.mediaCapabilities) {
    hide(g.navigator, 'mediaCapabilities', {
      decodingInfo: function (config) {
        return new Promise(function (resolve, reject) {
          var entry = config && (config.video || config.audio);
          if (!entry || typeof entry.contentType !== 'string') { reject(new TypeError("Failed to execute 'decodingInfo' on 'MediaCapabilities': The configuration dictionary has neither a video nor an audio member.")); return; }
          var ok = (config.type === 'media-source' || config.type === 'file') && __mse_supported(entry.contentType);
          if (ok && config.video && config.audio) ok = __mse_supported(config.audio.contentType);
          var info = { supported: ok, smooth: ok, powerEfficient: false };
          var ks = config.keySystemConfiguration;
          if (ok && ks) {   // encrypted: only Clear Key, and the configuration has to be one it can do
            if (ks.keySystem !== CLEARKEY) { resolve({ supported: false, smooth: false, powerEfficient: false }); return; }
            var request = { initDataType: ks.initDataType };
            requestAccess(CLEARKEY, [{ initDataTypes: ks.initDataType ? [ks.initDataType] : [], videoCapabilities: config.video ? [{ contentType: config.video.contentType, robustness: ks.video && ks.video.robustness || '' }] : [], audioCapabilities: config.audio ? [{ contentType: config.audio.contentType, robustness: ks.audio && ks.audio.robustness || '' }] : [], sessionTypes: ks.sessionTypes, persistentState: ks.persistentState, distinctiveIdentifier: ks.distinctiveIdentifier }]).then(function (access) {
              info.keySystemAccess = access;
              resolve(info);
            }, function () { resolve({ supported: false, smooth: false, powerEfficient: false }); });
            return;
          }
          resolve(info);
        });
      }
    });
  }

  // ---------------------------------------------------------------------------------------------------------------------------- Text tracks (WebVTT)
  // TextTrack, TextTrackList, TextTrackCue, VTTCue, VTTRegion, TrackEvent and the <track> element. The file is parsed by __vtt_parse (Media/WebVtt.cpp); the cues
  // of the tracks that are not disabled are handed to the page (__tt_publish) so that the player draws the showing ones, and the page says when the active ones
  // change (__tt_event), which is when "enter", "exit" and "cuechange" fire.
  var TT_KINDS = ['subtitles', 'captions', 'descriptions', 'chapters', 'metadata'];
  var TT_MODES = ['disabled', 'hidden', 'showing'];
  var V_VERTICAL = ['', 'rl', 'lr'], V_LINE_ALIGN = ['start', 'center', 'end'], V_POSITION_ALIGN = ['line-left', 'center', 'line-right', 'auto'];
  var V_ALIGN = ['start', 'center', 'end', 'left', 'right'];
  var ttSyntax = function (what, value, type) { return new DOMException("Failed to set the '" + what + "' property on '" + type + "': The provided value '" + value + "' is not a valid enum value.", 'SyntaxError'); };
  var ttRange = function (what, type) { return new DOMException("Failed to set the '" + what + "' property on '" + type + "': The value provided is outside the allowed range of 0 to 100.", 'IndexSizeError'); };
  var ttFinite = function (value, what, type) {
    value = Number(value);
    if (value !== value || value === Infinity || value === -Infinity) throw typeError("set the '" + what + "' property on '" + type + "'", 'The provided double value is non-finite.');
    return value;
  };
  var ttPercent = function (value, what, type) {
    value = ttFinite(value, what, type);
    if (value < 0 || value > 100) throw ttRange(what, type);
    return value;
  };

  // --- cues
  function TextTrackCue() { throw new TypeError('Illegal constructor'); }
  TextTrackCue.prototype = Object.create(EventTarget.prototype);
  method(TextTrackCue.prototype, 'constructor', TextTrackCue);
  var cueChanged = function (cue) { if (cue.__p.track) ttTrackChanged(cue.__p.track); };
  var cueProp = function (proto, name, type, get, set) {
    accessor(proto, name, function () { return get(this.__p); }, function (value) { set(this.__p, value, name, type); cueChanged(this); });
  };
  cueProp(TextTrackCue.prototype, 'id', 'TextTrackCue', function (p) { return p.id; }, function (p, v) { p.id = String(v); });
  cueProp(TextTrackCue.prototype, 'startTime', 'TextTrackCue', function (p) { return p.start; }, function (p, v, n, t) { p.start = ttFinite(v, n, t); });
  cueProp(TextTrackCue.prototype, 'endTime', 'TextTrackCue', function (p) { return p.end; }, function (p, v, n, t) { p.end = ttFinite(v, n, t); });
  cueProp(TextTrackCue.prototype, 'pauseOnExit', 'TextTrackCue', function (p) { return p.pause; }, function (p, v) { p.pause = !!v; });
  accessor(TextTrackCue.prototype, 'track', function () { return this.__p.track || null; });

  function VTTCue(startTime, endTime, text) {
    if (!(this instanceof VTTCue)) throw typeError("construct 'VTTCue'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (arguments.length < 3) throw typeError("construct 'VTTCue'", "3 arguments required, but only " + arguments.length + " present.");
    hide(this, '__p', { id: '', start: ttFinite(startTime, 'startTime', 'VTTCue'), end: ttFinite(endTime, 'endTime', 'VTTCue'), text: String(text), pause: false, vertical: 0, snap: true,
                        line: NaN, lineAlign: 0, position: NaN, positionAlign: 3, size: 100, align: 1, region: null, track: null });
    this.onenter = null;
    this.onexit = null;
  }
  VTTCue.prototype = Object.create(TextTrackCue.prototype);
  method(VTTCue.prototype, 'constructor', VTTCue);
  var enumProp = function (name, values) {
    cueProp(VTTCue.prototype, name, 'VTTCue', function (p) { return values[p[name]]; }, function (p, v, n, t) {
      var at = values.indexOf(String(v));
      if (at < 0) throw ttSyntax(n, v, t);
      p[name] = at;
    });
  };
  enumProp('vertical', V_VERTICAL);
  enumProp('lineAlign', V_LINE_ALIGN);
  enumProp('positionAlign', V_POSITION_ALIGN);
  enumProp('align', V_ALIGN);
  cueProp(VTTCue.prototype, 'snapToLines', 'VTTCue', function (p) { return p.snap; }, function (p, v) { p.snap = !!v; });
  cueProp(VTTCue.prototype, 'text', 'VTTCue', function (p) { return p.text; }, function (p, v) { p.text = String(v); });
  cueProp(VTTCue.prototype, 'line', 'VTTCue', function (p) { return p.line !== p.line ? 'auto' : p.line; }, function (p, v, n, t) { p.line = v === 'auto' ? NaN : ttFinite(v, n, t); });
  cueProp(VTTCue.prototype, 'position', 'VTTCue', function (p) { return p.position !== p.position ? 'auto' : p.position; }, function (p, v, n, t) { p.position = v === 'auto' ? NaN : ttPercent(v, n, t); });
  cueProp(VTTCue.prototype, 'size', 'VTTCue', function (p) { return p.size; }, function (p, v, n, t) { p.size = ttPercent(v, n, t); });
  cueProp(VTTCue.prototype, 'region', 'VTTCue', function (p) { return p.region; }, function (p, v) {
    if (v !== null && !(v instanceof VTTRegion)) throw typeError("set the 'region' property on 'VTTCue'", "The provided value is not of type 'VTTRegion'.");
    p.region = v;
  });
  method(VTTCue.prototype, 'getCueAsHTML', function () {
    var holder = g.document.createElement('div'), fragment = g.document.createDocumentFragment();
    holder.innerHTML = __vtt_html(this.__p.text);
    while (holder.firstChild) fragment.appendChild(holder.firstChild);
    return fragment;
  });
  // A cue made from what the parser found.
  var cueFromParsed = function (o, regions) {
    var cue = new VTTCue(o.start, o.end, o.text), p = cue.__p;
    p.id = o.id; p.pause = o.pause; p.vertical = o.vertical; p.snap = o.snap; p.line = o.line; p.lineAlign = o.lineAlign;
    p.position = o.position; p.positionAlign = o.positionAlign; p.size = o.size; p.align = o.align;
    if (o.region !== '') for (var i = 0; i < regions.length; i++) if (regions[i].__p.id === o.region) p.region = regions[i];
    return cue;
  };

  // --- regions
  function VTTRegion() {
    if (!(this instanceof VTTRegion)) throw typeError("construct 'VTTRegion'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    hide(this, '__p', { id: '', width: 100, lines: 3, ax: 0, ay: 100, vx: 0, vy: 100, scroll: false, tracks: [] });
  }
  method(VTTRegion.prototype, 'constructor', VTTRegion);
  var regionProp = function (name, key, set, get) {
    accessor(VTTRegion.prototype, name, get ? function () { return get(this.__p[key]); } : function () { return this.__p[key]; }, function (value) {
      this.__p[key] = set(value, name);
      for (var i = 0; i < this.__p.tracks.length; i++) ttTrackChanged(this.__p.tracks[i]);
    });
  };
  regionProp('id', 'id', function (v) { return String(v); });
  regionProp('width', 'width', function (v, n) { return ttPercent(v, n, 'VTTRegion'); });
  regionProp('lines', 'lines', function (v, n) { return ttFinite(v, n, 'VTTRegion') >>> 0; });
  regionProp('regionAnchorX', 'ax', function (v, n) { return ttPercent(v, n, 'VTTRegion'); });
  regionProp('regionAnchorY', 'ay', function (v, n) { return ttPercent(v, n, 'VTTRegion'); });
  regionProp('viewportAnchorX', 'vx', function (v, n) { return ttPercent(v, n, 'VTTRegion'); });
  regionProp('viewportAnchorY', 'vy', function (v, n) { return ttPercent(v, n, 'VTTRegion'); });
  regionProp('scroll', 'scroll', function (v, n) {
    v = String(v);
    if (v !== '' && v !== 'up') throw ttSyntax(n, v, 'VTTRegion');
    return v === 'up';
  }, function (b) { return b ? 'up' : ''; });
  var regionObject = function (r) { return r ? { id: r.__p.id, width: r.__p.width, lines: r.__p.lines, ax: r.__p.ax, ay: r.__p.ay, vx: r.__p.vx, vy: r.__p.vy, scroll: r.__p.scroll } : null; };

  // --- lists
  function TextTrackCueList() { throw new TypeError('Illegal constructor'); }
  method(TextTrackCueList.prototype, 'constructor', TextTrackCueList);
  accessor(TextTrackCueList.prototype, 'length', function () { return this.__items.length; });
  method(TextTrackCueList.prototype, 'item', function (index) { return this.__items[index >>> 0] || null; });
  method(TextTrackCueList.prototype, 'getCueById', function (id) {
    id = String(id);
    for (var i = 0; id !== '' && i < this.__items.length; i++) if (this.__items[i].__p.id === id) return this.__items[i];
    return null;
  });
  method(TextTrackCueList.prototype, ITER, function () { return this.__items.slice(); });
  // Makes a list hold these items (and the indexed properties that go with them).
  var listSet = function (list, items) {
    for (var i = items.length; i < list.__items.length; i++) delete list[i];
    list.__items = items;
    for (var k = 0; k < items.length; k++) list[k] = items[k];
  };
  var newCueList = function () { var list = Object.create(TextTrackCueList.prototype); hide(list, '__items', []); return list; };

  function VTTRegionList() { throw new TypeError('Illegal constructor'); }
  method(VTTRegionList.prototype, 'constructor', VTTRegionList);
  accessor(VTTRegionList.prototype, 'length', function () { return this.__items.length; });
  method(VTTRegionList.prototype, 'item', function (index) { return this.__items[index >>> 0] || null; });
  method(VTTRegionList.prototype, 'getRegionById', function (id) {
    id = String(id);
    for (var i = 0; i < this.__items.length; i++) if (this.__items[i].__p.id === id) return this.__items[i];
    return null;
  });
  method(VTTRegionList.prototype, ITER, function () { return this.__items.slice(); });
  var newRegionList = function () { var list = Object.create(VTTRegionList.prototype); hide(list, '__items', []); return list; };

  // --- tracks
  function TextTrack() { throw new TypeError('Illegal constructor'); }
  TextTrack.prototype = Object.create(EventTarget.prototype);
  method(TextTrack.prototype, 'constructor', TextTrack);
  var newTrack = function (kind, label, language, element) {
    var track = Object.create(TextTrack.prototype);
    hide(track, '__t', { kind: kind, label: label, language: language, element: element, mode: 0, modeSet: false, ready: 0, cues: [], active: [], list: newCueList(), regions: newRegionList(),
                         media: null, load: 0, src: null, styles: [] });
    track.oncuechange = null;
    return track;
  };
  var elementAttr = function (el, name) { var v = el.getAttribute(name); return v === null ? '' : v; };
  accessor(TextTrack.prototype, 'kind', function () {
    var t = this.__t;
    if (!t.element) return t.kind;
    var k = lower(elementAttr(t.element, 'kind'));
    return k === '' ? 'subtitles' : TT_KINDS.indexOf(k) < 0 ? 'metadata' : k;
  });
  accessor(TextTrack.prototype, 'label', function () { return this.__t.element ? elementAttr(this.__t.element, 'label') : this.__t.label; });
  accessor(TextTrack.prototype, 'language', function () { return this.__t.element ? elementAttr(this.__t.element, 'srclang') : this.__t.language; });
  accessor(TextTrack.prototype, 'id', function () { return this.__t.element ? elementAttr(this.__t.element, 'id') : ''; });
  accessor(TextTrack.prototype, 'inBandMetadataTrackDispatchType', function () { return ''; });
  accessor(TextTrack.prototype, 'mode', function () { return TT_MODES[this.__t.mode]; }, function (value) {
    var at = TT_MODES.indexOf(String(value));
    if (at >= 0) ttSetMode(this, at, true);
  });
  accessor(TextTrack.prototype, 'cues', function () { return this.__t.mode === 0 ? null : this.__t.list; });
  accessor(TextTrack.prototype, 'activeCues', function () {
    if (this.__t.mode === 0) return null;
    var list = newCueList();
    listSet(list, ttActiveOf(this));
    return list;
  });
  accessor(TextTrack.prototype, 'regions', function () { return this.__t.mode === 0 ? null : this.__t.regions; });
  method(TextTrack.prototype, 'addCue', function (cue) {
    if (arguments.length < 1) throw typeError("execute 'addCue' on 'TextTrack'", '1 argument required, but only 0 present.');
    if (!(cue instanceof TextTrackCue)) throw typeError("execute 'addCue' on 'TextTrack'", "parameter 1 is not of type 'TextTrackCue'.");
    var t = this.__t, old = cue.__p.track;
    if (old === this) return;
    if (old) old.removeCue(cue);
    t.cues.push(cue);
    cue.__p.track = this;
    ttTrackChanged(this);
  });
  method(TextTrack.prototype, 'removeCue', function (cue) {
    if (arguments.length < 1) throw typeError("execute 'removeCue' on 'TextTrack'", '1 argument required, but only 0 present.');
    if (!(cue instanceof TextTrackCue)) throw typeError("execute 'removeCue' on 'TextTrack'", "parameter 1 is not of type 'TextTrackCue'.");
    var t = this.__t, at = t.cues.indexOf(cue);
    if (at < 0) throw new DOMException("Failed to execute 'removeCue' on 'TextTrack': The specified cue is not listed in the TextTrack's list of cues.", 'NotFoundError');
    t.cues.splice(at, 1);
    cue.__p.track = null;
    var active = t.active.indexOf(cue);
    if (active >= 0) t.active.splice(active, 1);
    ttTrackChanged(this);
  });
  var ttDropRegion = function (track, at) {
    var list = track.__t.regions, region = list.__items[at], users = region.__p.tracks, items = list.__items.slice();
    items.splice(at, 1);
    listSet(list, items);
    if (users.indexOf(track) >= 0) users.splice(users.indexOf(track), 1);
  };
  method(TextTrack.prototype, 'addRegion', function (region) {
    if (!(region instanceof VTTRegion)) throw typeError("execute 'addRegion' on 'TextTrack'", "parameter 1 is not of type 'VTTRegion'.");
    var list = this.__t.regions, items = list.__items;
    for (var i = 0; i < items.length; i++) if (region.__p.id !== '' && items[i].__p.id === region.__p.id) { ttDropRegion(this, i); break; }
    var next = list.__items.slice();
    next.push(region);
    listSet(list, next);
    region.__p.tracks.push(this);
    ttTrackChanged(this);
  });
  method(TextTrack.prototype, 'removeRegion', function (region) {
    if (!(region instanceof VTTRegion)) throw typeError("execute 'removeRegion' on 'TextTrack'", "parameter 1 is not of type 'VTTRegion'.");
    var at = this.__t.regions.__items.indexOf(region);
    if (at < 0) throw new DOMException("Failed to execute 'removeRegion' on 'TextTrack': The specified region is not listed in the TextTrack's list of regions.", 'NotFoundError');
    ttDropRegion(this, at);
    ttTrackChanged(this);
  });

  // The cues of a track in the order of the standard: by start time, then end time, then as they were added (a stable sort; mostly sorted already).
  var ttSort = function (cues) {
    for (var i = 1; i < cues.length; i++) {
      var cue = cues[i], p = cue.__p, j = i - 1;
      while (j >= 0 && (cues[j].__p.start > p.start || (cues[j].__p.start === p.start && cues[j].__p.end > p.end))) { cues[j + 1] = cues[j]; j--; }
      cues[j + 1] = cue;
    }
  };
  var ttActiveOf = function (track) {
    var t = track.__t, out = [];
    if (t.mode === 0 || !t.media) return out;
    var now = t.media.currentTime;
    for (var i = 0; i < t.cues.length; i++) if (t.cues[i].__p.start <= now && now < t.cues[i].__p.end) out.push(t.cues[i]);
    return out;
  };
  var ttTrackChanged = function (track) {
    var t = track.__t;
    ttSort(t.cues);
    listSet(t.list, t.cues.slice());
    if (t.media) ttSchedulePublish(t.media);
  };
  var ttSchedulePublish = function (media) {
    var s = ttOf(media);
    if (s.queued) return;
    s.queued = true;
    setTimeout(function () {
      s.queued = false;
      var cues = [];
      for (var i = 0; i < s.list.__items.length; i++) {
        var t = s.list.__items[i].__t;
        if (t.mode === 0) continue;
        for (var k = 0; k < t.cues.length; k++) {
          var p = t.cues[k].__p;
          cues.push({ show: t.mode === 2, start: p.start, end: p.end, id: p.id, text: p.text, vertical: p.vertical, snap: p.snap, line: p.line, lineAlign: p.lineAlign, position: p.position,
                      positionAlign: p.positionAlign, size: p.size, align: p.align, region: regionObject(p.region) });
        }
      }
      __tt_publish(media, cues);
    }, 0);
  };
  var ttFire = function (target, event) { target.dispatchEvent(event); };

  // --- the tracks of a media element
  function TextTrackList() { throw new TypeError('Illegal constructor'); }
  TextTrackList.prototype = Object.create(EventTarget.prototype);
  method(TextTrackList.prototype, 'constructor', TextTrackList);
  accessor(TextTrackList.prototype, 'length', function () { return this.__items.length; });
  method(TextTrackList.prototype, 'item', function (index) { return this.__items[index >>> 0] || null; });
  method(TextTrackList.prototype, 'getTrackById', function (id) {
    id = String(id);
    for (var i = 0; i < this.__items.length; i++) if (this.__items[i].id === id) return this.__items[i];
    return null;
  });
  method(TextTrackList.prototype, ITER, function () { return this.__items.slice(); });

  function TrackEvent(type, init) {
    var event = new Event(type, init);
    Object.setPrototypeOf(event, TrackEvent.prototype);
    var track = init && init.track !== undefined ? init.track : null;
    if (track !== null && !(track instanceof TextTrack)) throw typeError("construct 'TrackEvent'", "The provided value is not of type '(VideoTrack or AudioTrack or TextTrack)'.");
    Object.defineProperty(event, 'track', { value: track, enumerable: true });
    return event;
  }
  TrackEvent.prototype = Object.create(Event.prototype);
  method(TrackEvent.prototype, 'constructor', TrackEvent);

  var ttOf = function (media) {
    var s = media.__tt;
    if (!s) {
      var list = Object.create(TextTrackList.prototype);
      hide(list, '__items', []);
      list.onaddtrack = null; list.onremovetrack = null; list.onchange = null;
      s = { list: list, queued: false };
      Object.defineProperty(media, '__tt', { value: s });
    }
    return s;
  };
  var ttAttach = function (media, track) {
    var list = ttOf(media).list, items = list.__items.slice();
    track.__t.media = media;
    items.push(track);
    listSet(list, items);
    ttSchedulePublish(media);
    setTimeout(function () { ttFire(list, new TrackEvent('addtrack', { track: track })); }, 0);
  };
  var ttDetach = function (media, track) {
    var list = ttOf(media).list, items = list.__items.slice(), at = items.indexOf(track);
    if (at < 0) return;
    items.splice(at, 1);
    listSet(list, items);
    track.__t.media = null;
    track.__t.active = [];
    ttSchedulePublish(media);
    setTimeout(function () { ttFire(list, new TrackEvent('removetrack', { track: track })); }, 0);
  };
  var ttSetMode = function (track, mode, byScript) {
    var t = track.__t;
    if (byScript) t.modeSet = true;
    if (t.mode === mode) return;
    t.mode = mode;
    if (mode === 0) t.active = [];
    ttLoad(track);
    ttTrackChanged(track);
    if (t.media) {
      var list = ttOf(t.media).list;
      setTimeout(function () { ttFire(list, new Event('change')); }, 0);
    }
  };

  // --- <track> elements
  var ttSrc = function (el) {
    var v = el.getAttribute('src');
    if (v === null || v === '') return '';
    try { return new URL(v, g.document.baseURI).href; } catch (e) { return v; }
  };
  var ttElementTrack = function (el) {
    var t = el.__trk;
    if (!t) {
      t = newTrack('subtitles', '', '', el);
      Object.defineProperty(el, '__trk', { value: t });
    }
    return t;
  };
  // Fetches and reads the file of a <track> whose mode is not "disabled".
  var ttLoad = function (track) {
    var t = track.__t, el = t.element;
    if (!el || t.mode === 0) return;
    var src = ttSrc(el);
    if (src === '' || (src === t.src && t.ready !== 0)) return;
    t.src = src;
    var id = ++t.load;
    t.ready = 1;
    t.cues = [];
    t.active = [];
    ttTrackChanged(track);
    var fail = function () {
      if (t.load !== id) return;
      t.ready = 3;
      ttFire(el, new Event('error'));
    };
    g.fetch(src).then(function (response) {
      if (!response.ok) throw new Error('status');
      return response.text();
    }).then(function (text) {
      if (t.load !== id) return;
      var parsed = __vtt_parse(text);
      if (!parsed) { fail(); return; }
      var regions = [], list = t.regions;
      listSet(list, []);
      for (var i = 0; i < parsed.regions.length; i++) {
        var r = new VTTRegion(), o = parsed.regions[i];
        r.__p.id = o.id; r.__p.width = o.width; r.__p.lines = o.lines; r.__p.ax = o.ax; r.__p.ay = o.ay; r.__p.vx = o.vx; r.__p.vy = o.vy; r.__p.scroll = o.scroll;
        r.__p.tracks.push(track);
        regions.push(r);
      }
      listSet(list, regions.slice());
      t.styles = parsed.styles;
      t.cues = [];
      for (var k = 0; k < parsed.cues.length; k++) {
        var cue = cueFromParsed(parsed.cues[k], regions);
        cue.__p.track = track;
        t.cues.push(cue);
      }
      t.ready = 2;
      ttTrackChanged(track);
      ttFire(el, new Event('load'));
    }).then(null, fail);
  };
  // Makes the text tracks of the <track> children of a media element (and drops those of children that are gone).
  var ttSync = function (media) {
    var list = ttOf(media).list, children = media.children, present = [];
    for (var i = 0; i < children.length; i++) if (lower(children[i].localName || children[i].tagName) === 'track') present.push(children[i]);
    for (var k = list.__items.length - 1; k >= 0; k--) {
      var existing = list.__items[k];
      if (existing.__t.element && present.indexOf(existing.__t.element) < 0) ttDetach(media, existing);
    }
    for (var n = 0; n < present.length; n++) {
      var el = present[n], track = ttElementTrack(el), t = track.__t;
      if (t.media !== media) {
        ttAttach(media, track);
        if (!t.modeSet && el.hasAttribute('default')) {   // (the "default" track is shown, unless another of its kind already is)
          var kind = track.kind, caption = kind === 'subtitles' || kind === 'captions', shown = false;
          for (var j = 0; j < list.__items.length; j++) {
            var other = list.__items[j], ok = other.kind;
            shown = shown || (other !== track && other.__t.mode === 2 && (ok === 'subtitles' || ok === 'captions') === caption);
          }
          if (!shown) ttSetMode(track, caption ? 2 : 1, false);
        }
      }
      if (t.mode !== 0 && ttSrc(el) !== t.src) ttLoad(track);
    }
  };
  // The cues that are active now, for each track: "exit" and "enter" at the cues, "cuechange" at the track and its element.
  var ttTick = function (media) {
    var list = ttOf(media).list, now = media.currentTime, pause = false;
    for (var i = 0; i < list.__items.length; i++) {
      var track = list.__items[i], t = track.__t, active = ttActiveOf(track), before = t.active, exited = [], entered = [], j;
      t.active = active;
      for (j = 0; j < before.length; j++) if (active.indexOf(before[j]) < 0) exited.push(before[j]);
      for (j = 0; j < active.length; j++) if (before.indexOf(active[j]) < 0) entered.push(active[j]);
      for (j = 0; j < exited.length; j++) {
        ttFire(exited[j], new Event('exit'));
        if (exited[j].__p.pause && !media.paused && exited[j].__p.end <= now) pause = true;
      }
      for (j = 0; j < entered.length; j++) ttFire(entered[j], new Event('enter'));
      if (exited.length !== 0 || entered.length !== 0) {
        ttFire(track, new Event('cuechange'));
        if (t.element) ttFire(t.element, new Event('cuechange'));
      }
    }
    if (pause) media.pause();
  };

  hide(g, '__text_tracks', function (media) {
    ttSync(media);
    return ttOf(media).list;
  });
  hide(g, '__tt_event', function (serial) {
    var media = __node_by_serial(serial);
    if (!media) return;
    ttSync(media);
    ttTick(media);
  });
  hide(g, '__track_get', function (el, key) {
    switch (key) {
      case 'kind': { var k = lower(elementAttr(el, 'kind')); return k === '' ? 'subtitles' : TT_KINDS.indexOf(k) < 0 ? 'metadata' : k; }
      case 'src': return ttSrc(el);
      case 'srclang': return elementAttr(el, 'srclang');
      case 'label': return elementAttr(el, 'label');
      case 'default': return el.hasAttribute('default');
      case 'readyState': return ttElementTrack(el).__t.ready;
      case 'track': return ttElementTrack(el);
    }
    return undefined;
  });
  hide(g, '__track_set', function (el, key, value) {
    if (key === 'default') { if (value) el.setAttribute('default', ''); else el.removeAttribute('default'); }
    else el.setAttribute(key, String(value));
  });
  if (g.HTMLMediaElement) {
    method(g.HTMLMediaElement.prototype, 'addTextTrack', function (kind, label, language) {
      if (arguments.length < 1) throw typeError("execute 'addTextTrack' on 'HTMLMediaElement'", '1 argument required, but only 0 present.');
      kind = String(kind);
      if (TT_KINDS.indexOf(kind) < 0) throw typeError("execute 'addTextTrack' on 'HTMLMediaElement'", "The provided value '" + kind + "' is not a valid enum value of type TextTrackKind.");
      var track = newTrack(kind, label === undefined ? '' : String(label), language === undefined ? '' : String(language), null);
      track.__t.mode = 1;
      track.__t.modeSet = true;
      track.__t.ready = 2;
      ttSync(this);
      ttAttach(this, track);
      return track;
    });
  }
  if (g.HTMLTrackElement) {
    var trackConstants = { NONE: 0, LOADING: 1, LOADED: 2, ERROR: 3 };
    for (var constant in trackConstants) { g.HTMLTrackElement[constant] = trackConstants[constant]; g.HTMLTrackElement.prototype[constant] = trackConstants[constant]; }
  }
  hide(g, 'TextTrack', TextTrack);
  hide(g, 'TextTrackList', TextTrackList);
  hide(g, 'TextTrackCue', TextTrackCue);
  hide(g, 'TextTrackCueList', TextTrackCueList);
  hide(g, 'VTTCue', VTTCue);
  hide(g, 'VTTRegion', VTTRegion);
  hide(g, 'VTTRegionList', VTTRegionList);
  hide(g, 'TrackEvent', TrackEvent);

  // ---------------------------------------------------------------------------------------------------------------------------- dashjs (DASH playback)
  // A player in the style of dash.js (dashjs.MediaPlayer().create(), initialize(), attachSource(), the quality and buffer calls, on()/off() with the events of
  // dashjs.MediaPlayer.events) over the browser's own DASH playback: a <video> whose src is a manifest is played natively (Media/DashClient.cpp), and this object
  // steers it through __dash_info / __dash_set and listens to the element.
  var DJ_EVENTS = {
    ERROR: 'error', MANIFEST_LOADED: 'manifestLoaded', STREAM_INITIALIZED: 'streamInitialized', QUALITY_CHANGE_REQUESTED: 'qualityChangeRequested',
    QUALITY_CHANGE_RENDERED: 'qualityChangeRendered', PLAYBACK_STARTED: 'playbackStarted', PLAYBACK_PAUSED: 'playbackPaused', PLAYBACK_ENDED: 'playbackEnded',
    PLAYBACK_SEEKING: 'playbackSeeking', PLAYBACK_SEEKED: 'playbackSeeked', PLAYBACK_TIME_UPDATED: 'playbackTimeUpdated', PLAYBACK_PROGRESS: 'playbackProgress',
    PLAYBACK_METADATA_LOADED: 'playbackMetaDataLoaded', PLAYBACK_ERROR: 'playbackError', PLAYBACK_PLAYING: 'playbackPlaying', PLAYBACK_WAITING: 'playbackWaiting',
    PLAYBACK_RATE_CHANGED: 'playbackRateChanged', PLAYBACK_VOLUME_CHANGED: 'playbackVolumeChanged', CAN_PLAY: 'canPlay', BUFFER_EMPTY: 'bufferStalled',
    BUFFER_LOADED: 'bufferLoaded', PLAYBACK_INITIALIZED: 'playbackInitialized', SOURCE_INITIALIZED: 'sourceInitialized', STREAM_TEARDOWN_COMPLETE: 'streamTeardownComplete'
  };
  // the element event -> the events of the player
  var DJ_ELEMENT = {
    play: 'PLAYBACK_STARTED', pause: 'PLAYBACK_PAUSED', ended: 'PLAYBACK_ENDED', seeking: 'PLAYBACK_SEEKING', seeked: 'PLAYBACK_SEEKED', timeupdate: 'PLAYBACK_TIME_UPDATED',
    progress: 'PLAYBACK_PROGRESS', loadedmetadata: 'PLAYBACK_METADATA_LOADED', playing: 'PLAYBACK_PLAYING', waiting: 'PLAYBACK_WAITING', canplay: 'CAN_PLAY',
    ratechange: 'PLAYBACK_RATE_CHANGED', volumechange: 'PLAYBACK_VOLUME_CHANGED'
  };
  var djPlayers = [];
  var djKinds = ['video', 'audio'];
  var djCheckType = function (type) {
    type = String(type);
    if (type !== 'video' && type !== 'audio') throw new Error('Invalid media type: ' + type);
    return type;
  };

  function djCreate() {
    var view = null, source = null, autoPlay = true, ready = false, listeners = {}, domHandlers = [], lastQuality = {}, held = {};
    var settings = { streaming: { abr: { autoSwitchBitrate: { video: true, audio: true } }, buffer: { bufferTimeAtTopQuality: 12 } }, debug: { logLevel: 3 } };
    var player = {};
    var fire = function (type, payload) {
      var list = listeners[type];
      if (!list) return;
      var event = { type: type };
      if (payload) for (var k in payload) event[k] = payload[k];
      list = list.slice();
      for (var i = 0; i < list.length; i++) {
        try { list[i].fn.call(list[i].scope, event); }
        catch (e) { (function (error) { setTimeout(function () { throw error; }, 0); }(e)); }
      }
    };
    var info = function () { return view ? __dash_info(view) : null; };
    var streamOf = function (type) {
      var i = info();
      if (!i) return null;
      for (var s = 0; s < i.streams.length; s++) if (i.streams[s].kind === type) { i.streams[s].index = s; return i.streams[s]; }
      return null;
    };
    var detachView = function () {
      for (var i = 0; i < domHandlers.length; i++) view.removeEventListener(domHandlers[i][0], domHandlers[i][1]);
      domHandlers = [];
      var at = djPlayers.indexOf(player);
      if (at >= 0) djPlayers.splice(at, 1);
      view = null;
    };
    // The browser says what its DASH playback did: it started on a manifest (init), or a stream changed quality.
    Object.defineProperty(player, '__element_event', { value: function (what, media) {
      if (media !== view) return;
      var i = info();
      if (!i) return;
      if (what === '__dash_init') {
        ready = true;
        fire(DJ_EVENTS.MANIFEST_LOADED, { data: { type: i.dynamic ? 'dynamic' : 'static', mediaPresentationDuration: i.duration, minBufferTime: i.minBufferTime, url: source } });
        fire(DJ_EVENTS.SOURCE_INITIALIZED, {});
        fire(DJ_EVENTS.STREAM_INITIALIZED, { streamInfo: { id: '0', index: 0, start: 0, duration: i.duration }, error: null });
        fire(DJ_EVENTS.PLAYBACK_INITIALIZED, {});
        for (var s = 0; s < i.streams.length; s++) lastQuality[i.streams[s].kind] = i.streams[s].quality;
      } else if (what === '__dash_quality') {
        for (var k = 0; k < i.streams.length; k++) {
          var st = i.streams[k], old = lastQuality[st.kind] === undefined ? -1 : lastQuality[st.kind];
          if (st.quality === old) continue;
          lastQuality[st.kind] = st.quality;
          var payload = { mediaType: st.kind, streamId: '0', oldQuality: old < 0 ? 0 : old, newQuality: st.quality };
          fire(DJ_EVENTS.QUALITY_CHANGE_REQUESTED, payload);
          fire(DJ_EVENTS.QUALITY_CHANGE_RENDERED, payload);
        }
      }
    } });
    var attachView = function (element) {
      if (view) detachView();
      view = element || null;
      if (!view) return;
      djPlayers.push(player);
      var listen = function (name, fn) { view.addEventListener(name, fn); domHandlers.push([name, fn]); };
      Object.keys(DJ_ELEMENT).forEach(function (name) {
        listen(name, function () {
          var payload = {};
          if (name === 'timeupdate') { payload.time = view.currentTime; payload.timeToEnd = view.duration - view.currentTime; }
          else if (name === 'seeking' || name === 'seeked') payload.seekTime = view.currentTime;
          else if (name === 'ratechange') payload.playbackRate = view.playbackRate;
          fire(DJ_EVENTS[DJ_ELEMENT[name]], payload);
          if (name === 'waiting') fire(DJ_EVENTS.BUFFER_EMPTY, { mediaType: 'video' });
          if (name === 'canplay') fire(DJ_EVENTS.BUFFER_LOADED, { mediaType: 'video' });
        });
      });
      listen('error', function () {
        var code = view.error ? view.error.code : 0;
        var payload = { error: { code: code, message: code === 2 ? 'network error' : code === 3 ? 'decode error' : 'source not supported', data: {} } };
        fire(DJ_EVENTS.ERROR, payload);
        fire(DJ_EVENTS.PLAYBACK_ERROR, payload);
      });
    };
    var attachSource = function (url) {
      if (!view) throw new Error('You must first call attachView() to set the video element before calling attachSource()');
      source = typeof url === 'string' ? url : url && url.url ? url.url : String(url);
      ready = false;
      lastQuality = {};
      view.src = source;
      if (autoPlay) { var p = view.play(); if (p && p.then) p.then(null, function () { }); }
    };
    var methods = {
      initialize: function (element, url, auto) { if (auto !== undefined) autoPlay = !!auto; if (element) attachView(element); if (url) attachSource(url); },
      attachView: attachView,
      attachSource: attachSource,
      getSource: function () { return source; },
      attachVideoContainer: function () { },
      attachTTMLRenderingDiv: function () { },
      isReady: function () { return !!view && ready; },
      getVideoElement: function () { if (!view) throw new Error('Video element not attached'); return view; },
      play: function () { var p = view.play(); if (p && p.then) p.then(null, function () { }); },
      pause: function () { view.pause(); },
      isPaused: function () { return view.paused; },
      isSeeking: function () { return view.seeking; },
      seek: function (t) { view.currentTime = Number(t); },
      time: function () { return view ? view.currentTime : 0; },
      duration: function () { var i = info(); return i && i.dynamic ? Infinity : view ? view.duration : NaN; },
      isDynamic: function () { var i = info(); return !!i && i.dynamic; },
      timeAsUTC: function () { return view ? view.currentTime : 0; },
      setAutoPlay: function (v) { autoPlay = !!v; },
      getAutoPlay: function () { return autoPlay; },
      setMute: function (v) { view.muted = !!v; },
      isMuted: function () { return view.muted; },
      setVolume: function (v) { view.volume = Number(v); },
      getVolume: function () { return view.volume; },
      setPlaybackRate: function (v) { view.playbackRate = Number(v); },
      getPlaybackRate: function () { return view.playbackRate; },
      getVersion: function () { return '4.7.0-native'; },
      getBufferLength: function (type) {
        var s = streamOf(type === undefined ? 'video' : djCheckType(type)) || streamOf('audio');
        return s ? s.buffer : NaN;
      },
      getBitrateInfoListFor: function (type) {
        var s = streamOf(djCheckType(type)), out = [];
        if (!s) return out;
        for (var i = 0; i < s.qualities.length; i++) {
          var q = s.qualities[i];
          out.push({ mediaType: type, bitrate: q.bandwidth, width: q.width, height: q.height, scanType: 'progressive', qualityIndex: i });
        }
        return out;
      },
      getTopBitrateInfoFor: function (type) { var list = methods.getBitrateInfoListFor(type); return list.length ? list[list.length - 1] : null; },
      getQualityFor: function (type) { var s = streamOf(djCheckType(type)); return s && s.quality >= 0 ? s.quality : 0; },
      setQualityFor: function (type, index) {
        var s = streamOf(djCheckType(type));
        if (!s) return;
        index = Number(index);
        if (!(index >= 0)) throw new Error('Quality index must be a non-negative number');
        __dash_set(view, s.index, 0, index);
        held[type] = true;
      },
      getAutoSwitchQualityFor: function (type) { var s = streamOf(djCheckType(type)); return s ? s.auto : settings.streaming.abr.autoSwitchBitrate[type]; },
      setAutoSwitchQualityFor: function (type, on) {
        type = djCheckType(type);
        settings.streaming.abr.autoSwitchBitrate[type] = !!on;
        var s = streamOf(type);
        if (s) __dash_set(view, s.index, 1, !!on);
      },
      getAverageThroughput: function () { var i = info(); return i ? i.throughput / 1000 : NaN; },
      getInitialBitrateFor: function () { return NaN; },
      setInitialBitrateFor: function () { },
      getMetricsFor: function () { return null; },
      getDashMetrics: function () { return { getCurrentBufferLevel: function (type) { return methods.getBufferLength(type); } }; },
      updateSettings: function (patch) {
        var merge = function (to, from) {
          for (var k in from) {
            if (from[k] !== null && typeof from[k] === 'object') { if (typeof to[k] !== 'object' || to[k] === null) to[k] = {}; merge(to[k], from[k]); }
            else to[k] = from[k];
          }
        };
        merge(settings, patch || {});
        var abr = patch && patch.streaming && patch.streaming.abr && patch.streaming.abr.autoSwitchBitrate;
        if (abr) for (var k in abr) if (k === 'video' || k === 'audio') { var s = streamOf(k); if (s) __dash_set(view, s.index, 1, !!abr[k]); }
      },
      getSettings: function () { return settings; },
      on: function (type, fn, scope) {
        if (!type || typeof fn !== 'function') throw new Error('Missing or invalid event type or listener');
        (listeners[type] = listeners[type] || []).push({ fn: fn, scope: scope });
      },
      off: function (type, fn, scope) {
        var list = listeners[type];
        if (!list) return;
        for (var i = list.length - 1; i >= 0; i--) if (list[i].fn === fn && list[i].scope === scope) list.splice(i, 1);
      },
      reset: function () {
        if (view) { view.pause(); view.removeAttribute('src'); view.load(); }
        source = null; ready = false; lastQuality = {};
        fire(DJ_EVENTS.STREAM_TEARDOWN_COMPLETE, {});
      },
      destroy: function () { methods.reset(); if (view) detachView(); listeners = {}; },
      getDebug: function () { return { setLogLevel: function () { }, getLogger: function () { return { debug: function () { }, info: function () { }, warn: function () { }, error: function () { } }; } }; },
      enableText: function () { },
      getVideoModel: function () { return { getElement: function () { return view; } }; }
    };
    for (var name in methods) Object.defineProperty(player, name, { value: methods[name], enumerable: true });
    return player;
  }
  function djMediaPlayer() {
    return { create: djCreate, extend: function () { }, getClassName: function () { return 'MediaPlayer'; } };
  }
  djMediaPlayer.events = DJ_EVENTS;
  djMediaPlayer.create = djCreate;
  hide(g, '__dash_event', function (serial, what) {
    var media = __node_by_serial(serial);
    if (!media) return;
    for (var i = 0; i < djPlayers.length; i++) djPlayers[i].__element_event(what, media);
  });
  if (!g.dashjs) hide(g, 'dashjs', { MediaPlayer: djMediaPlayer, supportsMediaSource: function () { return typeof g.MediaSource === 'function'; }, Version: '4.7.0-native' });

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

  // ------------------------------------------------------------------------------------------------------------------- unhandled rejections, idle callbacks
  function PromiseRejectionEvent(type, init) {
    if (!(this instanceof PromiseRejectionEvent)) throw typeError("construct 'PromiseRejectionEvent'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (arguments.length < 2 || init === null || typeof init !== 'object' || !('promise' in init)) throw typeError("construct 'PromiseRejectionEvent'", "required member promise is undefined.");
    var e = new Event(type, init);
    e.promise = init.promise;
    e.reason = init.reason;
    return e;
  }
  hide(g, 'PromiseRejectionEvent', PromiseRejectionEvent);
  hide(g, '__unhandled_rejection', function (promise, reason) {
    var e = new Event('unhandledrejection', { cancelable: true });
    e.promise = promise;
    e.reason = reason;
    g.dispatchEvent(e);
    return e.defaultPrevented === true;
  });
  hide(g, '__window_error', function (text) {
    var e = new Event('error', { cancelable: true });
    e.message = String(text);
    e.filename = String(g.location && g.location.href || '');
    e.lineno = 0;
    e.colno = 0;
    e.error = null;
    g.dispatchEvent(e);
  });
  var idleHandles = {}, idleNext = 1;
  hide(g, 'requestIdleCallback', function (callback, options) {
    if (typeof callback !== 'function') throw typeError("execute 'requestIdleCallback' on 'Window'", 'The callback provided as parameter 1 is not a function.');
    var id = idleNext++, started = Date.now();
    var timeout = options && typeof options.timeout === 'number' && options.timeout > 0 ? options.timeout : 0;
    idleHandles[id] = setTimeout(function () {
      delete idleHandles[id];
      var elapsed = Date.now() - started;
      callback({ didTimeout: timeout > 0 && elapsed >= timeout, timeRemaining: function () { return Math.max(0, 50 - (Date.now() - started - elapsed)); } });
    }, 1);
    return id;
  });
  hide(g, 'cancelIdleCallback', function (id) {
    if (idleHandles[id] !== undefined) { clearTimeout(idleHandles[id]); delete idleHandles[id]; }
  });

  // ------------------------------------------------------------------------------------------------------------------- WebSocket
  function CloseEvent(type, init) {
    if (!(this instanceof CloseEvent)) throw typeError("construct 'CloseEvent'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    var e = new Event(type, init);
    init = init || {};
    e.wasClean = !!init.wasClean; e.code = init.code | 0; e.reason = init.reason === undefined ? '' : String(init.reason);
    Object.setPrototypeOf(e, CloseEvent.prototype);
    return e;
  }
  CloseEvent.prototype = Object.create(Event.prototype);
  method(CloseEvent.prototype, 'constructor', CloseEvent);
  hide(g, 'CloseEvent', CloseEvent);
  function WebSocket(url, protocols) {
    if (!(this instanceof WebSocket)) throw typeError("construct 'WebSocket'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (arguments.length < 1) throw typeError("construct 'WebSocket'", "1 argument required, but only 0 present.");
    var parsed;
    try { parsed = new URL(String(url), location.href); } catch (e) { throw new DOMException("Failed to construct 'WebSocket': The URL '" + url + "' is invalid.", 'SyntaxError'); }
    var scheme = parsed.protocol;
    if (scheme === 'http:') scheme = 'ws:'; else if (scheme === 'https:') scheme = 'wss:';
    if (scheme !== 'ws:' && scheme !== 'wss:') throw new DOMException("Failed to construct 'WebSocket': The URL's scheme must be either 'ws' or 'wss'. '" + parsed.protocol.slice(0, -1) + "' is not allowed.", 'SyntaxError');
    if (parsed.hash) throw new DOMException("Failed to construct 'WebSocket': The URL contains a fragment identifier ('" + parsed.hash + "'). Fragment identifiers are not allowed in WebSocket URLs.", 'SyntaxError');
    if (location.protocol === 'https:' && scheme === 'ws:' && parsed.hostname !== 'localhost' && parsed.hostname !== '127.0.0.1')
      throw new DOMException("Failed to construct 'WebSocket': An insecure WebSocket connection may not be initiated from a page loaded over HTTPS.", 'SecurityError');
    var list = protocols === undefined ? [] : (typeof protocols === 'string' ? [protocols] : Array.prototype.slice.call(protocols).map(String));
    for (var i = 0; i < list.length; i++) {
      if (list[i] === '' || /[\u0000- \u007f()<>@,;:\\"\/\[\]?={}]/.test(list[i])) throw new DOMException("Failed to construct 'WebSocket': The subprotocol '" + list[i] + "' is invalid.", 'SyntaxError');
      if (list.indexOf(list[i]) !== i) throw new DOMException("Failed to construct 'WebSocket': The subprotocol '" + list[i] + "' is duplicated.", 'SyntaxError');
    }
    this.url = scheme + parsed.href.slice(parsed.protocol.length);
    this.readyState = 0; this.bufferedAmount = 0; this.extensions = ''; this.protocol = ''; this.binaryType = 'blob';
    this.onopen = null; this.onmessage = null; this.onerror = null; this.onclose = null;
    var self = this, id = __ws_open(this.url, list.join(', '));
    hide(this, '__id', id);
    if (!id) {   // it could not be started: the failure is told after the script that asked
      var reason = __ws_error;
      setTimeout(function () { socketEnded(self, false, 1006, ''); }, 0);
      return;
    }
    hide(this, '__timer', setInterval(function () { pump(self); }, 10));
  }
  WebSocket.prototype = Object.create(EventTarget.prototype);
  method(WebSocket.prototype, 'constructor', WebSocket);
  WebSocket.CONNECTING = WebSocket.prototype.CONNECTING = 0; WebSocket.OPEN = WebSocket.prototype.OPEN = 1;
  WebSocket.CLOSING = WebSocket.prototype.CLOSING = 2; WebSocket.CLOSED = WebSocket.prototype.CLOSED = 3;
  function socketEnded(ws, opened, code, reason) {
    if (ws.readyState === 3) return;
    ws.readyState = 3;
    if (ws.__timer) { clearInterval(ws.__timer); ws.__timer = 0; }
    if (!opened) ws.dispatchEvent(new Event('error'));
    ws.dispatchEvent(new CloseEvent('close', { wasClean: opened && code !== 1006, code: code, reason: reason }));
    if (ws.__id) __ws_release(ws.__id);
  }
  function pump(ws) {
    if (ws.readyState === 3) return;
    var id = ws.__id, state = __ws_state(id);
    if (ws.readyState === 0 && state >= 1 && (state < 3 || __ws_info(id)[3])) {
      ws.readyState = state === 1 ? 1 : 2;
      ws.protocol = __ws_info(id)[0];
      ws.dispatchEvent(new Event('open'));
    }
    var data, guard = 0;
    while (ws.readyState !== 3 && guard++ < 100 && (data = __ws_receive(id)) !== null) {
      if (typeof data !== 'string') {
        if (ws.binaryType === 'arraybuffer') data = data.buffer;
        else data = new Blob([data]);
      }
      ws.dispatchEvent(new MessageEvent('message', { data: data, origin: new URL(ws.url.replace(/^ws/, 'http')).origin, lastEventId: '', source: null, ports: [] }));
    }
    if (ws.readyState === 1 && state === 2) ws.readyState = 2;
    if (state === 3) {
      var info = __ws_info(id);
      socketEnded(ws, info[3], info[1], info[2]);
    }
  }
  method(WebSocket.prototype, 'send', function (data) {
    if (this.readyState === 0) throw new DOMException("Failed to execute 'send' on 'WebSocket': Still in CONNECTING state.", 'InvalidStateError');
    if (this.readyState !== 1) return;
    var self = this;
    if (data instanceof Blob) { data.arrayBuffer().then(function (buffer) { if (self.readyState === 1) __ws_send(self.__id, new Uint8Array(buffer)); }); return; }
    if (typeof data === 'string') { __ws_send(this.__id, data); return; }
    if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) { __ws_send(this.__id, data instanceof ArrayBuffer ? new Uint8Array(data) : data); return; }
    __ws_send(this.__id, String(data));
  });
  method(WebSocket.prototype, 'close', function (code, reason) {
    if (code !== undefined && code !== 1000 && !(code >= 3000 && code <= 4999))
      throw new DOMException("Failed to execute 'close' on 'WebSocket': The code must be either 1000, or between 3000 and 4999. " + code + ' is neither.', 'InvalidAccessError');
    reason = reason === undefined ? '' : String(reason);
    if (__utf8_bytes_length(reason) > 123) throw new DOMException("Failed to execute 'close' on 'WebSocket': The message must not be greater than 123 bytes.", 'SyntaxError');
    if (this.readyState >= 2) return;
    this.readyState = 2;
    if (this.__id) __ws_close(this.__id, code === undefined ? 1000 : code, reason);
  });
  hide(g, 'WebSocket', WebSocket);

  // ------------------------------------------------------------------------------------------------------------------- Worker
  var workers = {};
  function Worker(url, options) {
    if (!(this instanceof Worker)) throw typeError("construct 'Worker'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (arguments.length < 1) throw typeError("construct 'Worker'", "1 argument required, but only 0 present.");
    var address;
    try { address = new URL(String(url), location.href); } catch (e) { throw new DOMException("Failed to construct 'Worker': Script at '" + url + "' cannot be accessed from origin '" + location.origin + "'.", 'SyntaxError'); }
    if (address.protocol !== 'blob:' && address.protocol !== 'data:' && address.origin !== location.origin)
      throw new DOMException("Failed to construct 'Worker': Script at '" + address.href + "' cannot be accessed from origin '" + location.origin + "'.", 'SecurityError');
    this.onmessage = null; this.onerror = null; this.onmessageerror = null;
    hide(this, '__id', 0);
    hide(this, '__pending', []);
    hide(this, '__dead', false);
    hide(this, '__timer', 0);
    var self = this, name = options && options.name !== undefined ? String(options.name) : '';
    var source = address.protocol === 'blob:' ? (blobUrls[address.href] ? blobUrls[address.href].text() : Promise.reject(new Error('The blob is gone'))) : fetch(address.href).then(function (r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); });
    source.then(function (text) {
      if (self.__dead) return;
      var id = __worker_start(text, address.href, name);
      if (!id) { fail(self, __worker_error || 'The worker could not be started'); return; }
      self.__id = id;
      workers[id] = self;
      self.__pending.forEach(function (m) { __worker_post(id, m); });
      self.__pending.length = 0;
      self.__timer = setInterval(pumpWorkers, 5);
    }).catch(function (e) { fail(self, e && e.message ? e.message : String(e)); });
  }
  Worker.prototype = Object.create(EventTarget.prototype);
  method(Worker.prototype, 'constructor', Worker);
  function fail(worker, message) {
    if (worker.__dead) return;
    var e = new Event('error', { cancelable: true });
    e.message = String(message); e.filename = ''; e.lineno = 0; e.colno = 0; e.error = null;
    worker.dispatchEvent(e);
  }
  method(Worker.prototype, 'postMessage', function (message) {
    if (arguments.length < 1) throw typeError("execute 'postMessage' on 'Worker'", "1 argument required, but only 0 present.");
    if (this.__dead) return;
    var text = __wire_encode(message);
    if (this.__id) __worker_post(this.__id, text); else this.__pending.push(text);
  });
  method(Worker.prototype, 'terminate', function () {
    if (this.__dead) return;
    this.__dead = true;
    if (this.__id) { __worker_terminate(this.__id); delete workers[this.__id]; }
    if (this.__timer) { clearInterval(this.__timer); this.__timer = 0; }
  });
  var pumping = false;
  function pumpWorkers() {
    if (pumping) return;
    pumping = true;
    try {
      var events = __worker_pump();
      for (var i = 0; i < events.length; i++) {
        var id = events[i][0], kind = events[i][1], text = events[i][2], worker = workers[id];
        if (!worker || worker.__dead) continue;
        if (kind === 0) {
          var data;
          try { data = __wire_decode(text); } catch (e) { worker.dispatchEvent(new MessageEvent('messageerror', { data: null })); continue; }
          worker.dispatchEvent(new MessageEvent('message', { data: data, origin: '', lastEventId: '', source: null, ports: [] }));
        } else if (kind === 1) {
          fail(worker, text);
        } else {
          worker.terminate();
        }
      }
    } finally { pumping = false; }
    var any = false;
    for (var k in workers) any = true;
  }
  hide(g, 'Worker', Worker);

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


const char kDomPrelude[] = R"JSPRELUDE((function (g) {
  var hide = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true }); };
  var method = function (proto, name, fn) { Object.defineProperty(proto, name, { value: fn, writable: true }); };
  var illegal = function (name) { var f = function () { throw new TypeError('Illegal constructor'); }; Object.defineProperty(f, 'name', { value: name }); return f; };
  var EP = Element.prototype, NP = Node.prototype, TP = Text.prototype, DP = Document.prototype;
  var doc = document;
  var indexOf = function (node) { var i = 0; for (var c = node.parentNode.firstChild; c && c !== node; c = c.nextSibling) i++; return i; };
  var nodeLength = function (node) { return node.nodeType === 3 || node.nodeType === 8 ? node.data.length : node.childNodes.length; };
  var isInclusiveAncestor = function (a, b) { for (var n = b; n; n = n.parentNode) if (n === a) return true; return false; };
  var dom = function (message, name) { return new DOMException(message, name); };

  // ------------------------------------------------------------------------------------------------------------------ event handler properties
  ['onclick', 'ondblclick', 'onmousedown', 'onmouseup', 'onmousemove', 'onmouseover', 'onmouseout', 'onmouseenter', 'onmouseleave', 'oncontextmenu', 'onwheel', 'onkeydown', 'onkeyup',
   'onkeypress', 'oninput', 'onchange', 'onfocus', 'onblur', 'onsubmit', 'onreset', 'oninvalid', 'onselect', 'onscroll', 'onload', 'onerror', 'onabort', 'ondrag', 'ondragstart', 'ondragend',
   'ondragover', 'ondragenter', 'ondragleave', 'ondrop', 'ontouchstart', 'ontouchend', 'ontouchmove', 'ontouchcancel', 'onpointerdown', 'onpointerup', 'onpointermove', 'onpointerover',
   'onpointerout', 'onpointerenter', 'onpointerleave', 'onpointercancel', 'ontransitionend', 'onanimationend', 'oncopy', 'oncut', 'onpaste', 'oncanplay', 'onplay', 'onpause', 'onended',
   'ontimeupdate', 'onvolumechange', 'onencrypted', 'onwaitingforkey', 'onloadstart', 'onloadedmetadata', 'onloadeddata', 'oncanplaythrough', 'onplaying', 'onwaiting', 'onseeking', 'onseeked', 'ondurationchange', 'onratechange', 'onprogress', 'onsuspend', 'onstalled', 'onemptied', 'ontoggle', 'onauxclick', 'ongotpointercapture', 'onlostpointercapture'].forEach(function (name) { Object.defineProperty(EP, name, { value: null, writable: true }); });
  ['onerror', 'onresize', 'onhashchange', 'onpopstate', 'onmessage', 'onmessageerror', 'onstorage', 'onbeforeunload', 'onunload', 'ononline', 'onoffline', 'onfocus', 'onblur', 'onpageshow',
   'onpagehide', 'onunhandledrejection', 'onrejectionhandled', 'onlanguagechange', 'onclick', 'onkeydown', 'onkeyup', 'oninput', 'onchange', 'onsubmit', 'onscroll', 'onwheel'].forEach(function (name) {
    if (g[name] === undefined) g[name] = null;
  });
  if (doc.onreadystatechange === undefined) doc.onreadystatechange = null;

  // ------------------------------------------------------------------------------------------------------------------ Node extras
  function sameAttributes(a, b) {
    var an = a.getAttributeNames(), bn = b.getAttributeNames();
    if (an.length !== bn.length) return false;
    for (var i = 0; i < an.length; i++) if (a.getAttribute(an[i]) !== b.getAttribute(an[i])) return false;
    return true;
  }
  method(NP, 'isEqualNode', function (other) {
    if (other === null || other === undefined) return false;
    if (this === other) return true;
    if (this.nodeType !== other.nodeType) return false;
    if (this.nodeType === 1) {
      if (this.localName !== other.localName || this.namespaceURI !== other.namespaceURI || !sameAttributes(this, other)) return false;
    } else if (this.nodeType === 3 || this.nodeType === 8) {
      if (this.data !== other.data) return false;
    }
    var a = this.firstChild, b = other.firstChild;
    while (a && b) { if (!a.isEqualNode(b)) return false; a = a.nextSibling; b = b.nextSibling; }
    return !a && !b;
  });
  method(NP, 'lookupNamespaceURI', function (prefix) { return prefix === null || prefix === '' ? (this.nodeType === 1 ? this.namespaceURI : null) : (prefix === 'xml' ? 'http://www.w3.org/XML/1998/namespace' : null); });
  method(NP, 'lookupPrefix', function () { return null; });
  method(NP, 'isDefaultNamespace', function (ns) { return (ns === null || ns === '') ? !this.namespaceURI : this.namespaceURI === ns; });

  // ------------------------------------------------------------------------------------------------------------------ attributes as nodes
  function Attr() { throw new TypeError('Illegal constructor'); }
  hide(g, 'Attr', Attr);
  function makeAttr(name, value, owner, ns) {
    var a = Object.create(Attr.prototype);
    a.name = name; a.localName = name.indexOf(':') >= 0 ? name.slice(name.indexOf(':') + 1) : name; a.prefix = name.indexOf(':') >= 0 ? name.slice(0, name.indexOf(':')) : null;
    a.namespaceURI = ns || null; a.value = String(value); a.nodeValue = a.value; a.textContent = a.value; a.ownerElement = owner || null; a.specified = true; a.nodeType = 2; a.nodeName = name;
    return a;
  }
  method(Attr.prototype, 'toString', function () { return '[object Attr]'; });
  method(EP, 'getAttributeNode', function (name) { return this.hasAttribute(name) ? makeAttr(String(name).toLowerCase(), this.getAttribute(name), this) : null; });
  method(EP, 'getAttributeNodeNS', function (ns, name) { return this.hasAttribute(name) ? makeAttr(name, this.getAttribute(name), this, ns) : null; });
  method(EP, 'setAttributeNode', function (attr) {
    var old = this.getAttributeNode(attr.name);
    this.setAttribute(attr.name, attr.value);
    attr.ownerElement = this;
    return old;
  });
  method(EP, 'setAttributeNodeNS', EP.setAttributeNode);
  method(EP, 'removeAttributeNode', function (attr) {
    if (!this.hasAttribute(attr.name)) throw dom("Failed to execute 'removeAttributeNode' on 'Element': The node provided is not an attribute of this element.", 'NotFoundError');
    var old = this.getAttributeNode(attr.name);
    this.removeAttribute(attr.name);
    return old;
  });
  method(EP, 'getAttributeNS', function (ns, name) { return this.getAttribute(name); });
  method(EP, 'setAttributeNS', function (ns, qname, value) { this.setAttribute(qname, value); });
  method(EP, 'removeAttributeNS', function (ns, name) { this.removeAttribute(name); });
  method(EP, 'hasAttributeNS', function (ns, name) { return this.hasAttribute(name); });
  method(DP, 'createAttribute', function (name) { return makeAttr(String(name).toLowerCase(), '', null); });
  method(DP, 'createAttributeNS', function (ns, name) { return makeAttr(String(name), '', null, ns); });

  // ------------------------------------------------------------------------------------------------------------------ CharacterData
  var CD = [TP, Comment.prototype];
  CD.forEach(function (P) {
    method(P, 'substringData', function (offset, count) {
      if (offset > this.data.length) throw dom("Failed to execute 'substringData' on 'CharacterData': The offset " + offset + ' is greater than the node\'s length (' + this.data.length + ').', 'IndexSizeError');
      return this.data.substr(offset, count);
    });
    method(P, 'replaceData', function (offset, count, text) {
      if (offset > this.data.length) throw dom("Failed to execute 'replaceData' on 'CharacterData': The offset " + offset + ' is greater than the node\'s length (' + this.data.length + ').', 'IndexSizeError');
      this.data = this.data.slice(0, offset) + String(text) + this.data.slice(offset + count);
    });
    method(P, 'appendData', function (text) { this.data = this.data + String(text); });
    method(P, 'insertData', function (offset, text) { this.replaceData(offset, 0, text); });
    method(P, 'deleteData', function (offset, count) { this.replaceData(offset, count, ''); });
  });

  // ------------------------------------------------------------------------------------------------------------------ NodeFilter, TreeWalker, NodeIterator
  var NodeFilter = {
    FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3,
    SHOW_ALL: 4294967295, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_ENTITY_REFERENCE: 16, SHOW_ENTITY: 32, SHOW_PROCESSING_INSTRUCTION: 64,
    SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024, SHOW_NOTATION: 2048
  };
  hide(g, 'NodeFilter', NodeFilter);
  function accepts(walker, node) {
    var bit = Math.pow(2, node.nodeType - 1);
    if (Math.floor(walker.whatToShow / bit) % 2 !== 1) return 3;
    var f = walker.filter;
    if (!f) return 1;
    if (walker.__active) throw dom('The filter is already running.', 'InvalidStateError');
    walker.__active = true;
    try { return (typeof f === 'function' ? f(node) : f.acceptNode(node)) | 0; } finally { walker.__active = false; }
  }
  function TreeWalker() { throw new TypeError('Illegal constructor'); }
  hide(g, 'TreeWalker', TreeWalker);
  var TW = TreeWalker.prototype;
  function children(w, first) {
    var node = first ? w.currentNode.firstChild : w.currentNode.lastChild;
    while (node) {
      var r = accepts(w, node);
      if (r === 1) { w.currentNode = node; return node; }
      if (r === 3) { var child = first ? node.firstChild : node.lastChild; if (child) { node = child; continue; } }
      while (node) {
        var sib = first ? node.nextSibling : node.previousSibling;
        if (sib) { node = sib; break; }
        var p = node.parentNode;
        if (!p || p === w.root || p === w.currentNode) return null;
        node = p;
      }
    }
    return null;
  }
  function siblings(w, next) {
    var node = w.currentNode;
    if (node === w.root) return null;
    while (true) {
      var sib = next ? node.nextSibling : node.previousSibling;
      while (sib) {
        node = sib;
        var r = accepts(w, node);
        if (r === 1) { w.currentNode = node; return node; }
        sib = next ? node.firstChild : node.lastChild;
        if (r === 2 || !sib) sib = next ? node.nextSibling : node.previousSibling;
      }
      node = node.parentNode;
      if (!node || node === w.root) return null;
      if (accepts(w, node) === 1) return null;
    }
  }
  method(TW, 'parentNode', function () {
    var node = this.currentNode;
    while (node && node !== this.root) {
      node = node.parentNode;
      if (node && accepts(this, node) === 1) { this.currentNode = node; return node; }
    }
    return null;
  });
  method(TW, 'firstChild', function () { return children(this, true); });
  method(TW, 'lastChild', function () { return children(this, false); });
  method(TW, 'nextSibling', function () { return siblings(this, true); });
  method(TW, 'previousSibling', function () { return siblings(this, false); });
  method(TW, 'nextNode', function () {
    var node = this.currentNode, result = 1;
    while (true) {
      while (result !== 2 && node.firstChild) {
        node = node.firstChild;
        result = accepts(this, node);
        if (result === 1) { this.currentNode = node; return node; }
      }
      var sibling = null, temp = node;
      while (temp) {
        if (temp === this.root) return null;
        sibling = temp.nextSibling;
        if (sibling) { node = sibling; break; }
        temp = temp.parentNode;
      }
      if (!sibling) return null;
      result = accepts(this, node);
      if (result === 1) { this.currentNode = node; return node; }
    }
  });
  method(TW, 'previousNode', function () {
    var node = this.currentNode;
    while (node !== this.root) {
      var sibling = node.previousSibling;
      while (sibling) {
        node = sibling;
        var result = accepts(this, node);
        while (result !== 2 && node.lastChild) { node = node.lastChild; result = accepts(this, node); }
        if (result === 1) { this.currentNode = node; return node; }
        sibling = node.previousSibling;
      }
      if (node === this.root || !node.parentNode) return null;
      node = node.parentNode;
      if (accepts(this, node) === 1) { this.currentNode = node; return node; }
    }
    return null;
  });
  method(TW, 'toString', function () { return '[object TreeWalker]'; });
  function filterArg(filter) { return filter === undefined ? null : filter; }
  method(DP, 'createTreeWalker', function (root, whatToShow, filter) {
    if (!root || typeof root.nodeType !== 'number') throw new TypeError("Failed to execute 'createTreeWalker' on 'Document': parameter 1 is not of type 'Node'.");
    var w = Object.create(TW);
    w.root = root; w.whatToShow = whatToShow === undefined ? 4294967295 : whatToShow >>> 0; w.filter = filterArg(filter); w.currentNode = root;
    hide(w, '__active', false);
    return w;
  });
  function NodeIterator() { throw new TypeError('Illegal constructor'); }
  hide(g, 'NodeIterator', NodeIterator);
  var NI = NodeIterator.prototype;
  function following(node, root) {
    if (node.firstChild) return node.firstChild;
    while (node) {
      if (node === root) return null;
      if (node.nextSibling) return node.nextSibling;
      node = node.parentNode;
    }
    return null;
  }
  function preceding(node, root) {
    if (node === root) return null;
    var s = node.previousSibling;
    if (!s) return node.parentNode;
    while (s.lastChild) s = s.lastChild;
    return s;
  }
  function iterate(it, next) {
    var node = it.referenceNode, before = it.pointerBeforeReferenceNode;
    while (true) {
      if (next) {
        if (!before) { node = following(node, it.root); if (!node) return null; } else before = false;
      } else {
        if (before) { node = preceding(node, it.root); if (!node) return null; } else before = true;
      }
      if (accepts(it, node) === 1) break;
    }
    it.referenceNode = node; it.pointerBeforeReferenceNode = before;
    return node;
  }
  method(NI, 'nextNode', function () { return iterate(this, true); });
  method(NI, 'previousNode', function () { return iterate(this, false); });
  method(NI, 'detach', function () {});
  method(NI, 'toString', function () { return '[object NodeIterator]'; });
  method(DP, 'createNodeIterator', function (root, whatToShow, filter) {
    if (!root || typeof root.nodeType !== 'number') throw new TypeError("Failed to execute 'createNodeIterator' on 'Document': parameter 1 is not of type 'Node'.");
    var it = Object.create(NI);
    it.root = root; it.whatToShow = whatToShow === undefined ? 4294967295 : whatToShow >>> 0; it.filter = filterArg(filter); it.referenceNode = root; it.pointerBeforeReferenceNode = true;
    hide(it, '__active', false);
    return it;
  });

  // ------------------------------------------------------------------------------------------------------------------ Range
  function cmpPoints(a, ao, b, bo) {
    if (a === b) return ao < bo ? -1 : ao > bo ? 1 : 0;
    var pos = a.compareDocumentPosition(b);
    if (pos & 4) {
      if (pos & 16) {
        var c = b;
        while (c.parentNode !== a) c = c.parentNode;
        return indexOf(c) < ao ? 1 : -1;
      }
      return -1;
    }
    return -cmpPoints(b, bo, a, ao);
  }
  function rangeRoot(node) { while (node.parentNode) node = node.parentNode; return node; }
  function checkOffset(node, offset, what) {
    if (node.nodeType === 10) throw dom("Failed to execute '" + what + "' on 'Range': The node provided is a doctype.", 'InvalidNodeTypeError');
    if (offset > nodeLength(node)) throw dom("Failed to execute '" + what + "' on 'Range': The offset " + offset + ' is larger than the node\'s length (' + nodeLength(node) + ').', 'IndexSizeError');
  }
  function Range() {
    if (!(this instanceof Range)) throw new TypeError("Failed to construct 'Range': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    this.startContainer = doc; this.startOffset = 0; this.endContainer = doc; this.endOffset = 0;
    this.collapsed = true; this.commonAncestorContainer = doc;
  }
  hide(g, 'Range', Range);
  var RP = Range.prototype;
  Range.START_TO_START = 0; Range.START_TO_END = 1; Range.END_TO_END = 2; Range.END_TO_START = 3;
  function refresh(r) {
    r.collapsed = r.startContainer === r.endContainer && r.startOffset === r.endOffset;
    var n = r.startContainer;
    while (n && !isInclusiveAncestor(n, r.endContainer)) n = n.parentNode;
    r.commonAncestorContainer = n || r.startContainer;
  }
  method(RP, 'setStart', function (node, offset) {
    checkOffset(node, offset, 'setStart');
    this.startContainer = node; this.startOffset = offset;
    if (rangeRoot(this.endContainer) !== rangeRoot(node) || cmpPoints(node, offset, this.endContainer, this.endOffset) > 0) { this.endContainer = node; this.endOffset = offset; }
    refresh(this);
  });
  method(RP, 'setEnd', function (node, offset) {
    checkOffset(node, offset, 'setEnd');
    this.endContainer = node; this.endOffset = offset;
    if (rangeRoot(this.startContainer) !== rangeRoot(node) || cmpPoints(node, offset, this.startContainer, this.startOffset) < 0) { this.startContainer = node; this.startOffset = offset; }
    refresh(this);
  });
  method(RP, 'setStartBefore', function (node) { this.setStart(node.parentNode, indexOf(node)); });
  method(RP, 'setStartAfter', function (node) { this.setStart(node.parentNode, indexOf(node) + 1); });
  method(RP, 'setEndBefore', function (node) { this.setEnd(node.parentNode, indexOf(node)); });
  method(RP, 'setEndAfter', function (node) { this.setEnd(node.parentNode, indexOf(node) + 1); });
  method(RP, 'collapse', function (toStart) {
    if (toStart) { this.endContainer = this.startContainer; this.endOffset = this.startOffset; } else { this.startContainer = this.endContainer; this.startOffset = this.endOffset; }
    refresh(this);
  });
  method(RP, 'selectNode', function (node) {
    if (!node.parentNode) throw dom("Failed to execute 'selectNode' on 'Range': the given Node has no parent.", 'InvalidNodeTypeError');
    var p = node.parentNode, i = indexOf(node);
    this.startContainer = p; this.startOffset = i; this.endContainer = p; this.endOffset = i + 1; refresh(this);
  });
  method(RP, 'selectNodeContents', function (node) {
    this.startContainer = node; this.startOffset = 0; this.endContainer = node; this.endOffset = nodeLength(node); refresh(this);
  });
  method(RP, 'cloneRange', function () {
    var r = new Range();
    r.startContainer = this.startContainer; r.startOffset = this.startOffset; r.endContainer = this.endContainer; r.endOffset = this.endOffset; refresh(r);
    return r;
  });
  method(RP, 'detach', function () {});
  method(RP, 'compareBoundaryPoints', function (how, other) {
    var a, ao, b, bo;
    if (how === 0) { a = this.startContainer; ao = this.startOffset; b = other.startContainer; bo = other.startOffset; }
    else if (how === 1) { a = this.endContainer; ao = this.endOffset; b = other.startContainer; bo = other.startOffset; }
    else if (how === 2) { a = this.endContainer; ao = this.endOffset; b = other.endContainer; bo = other.endOffset; }
    else if (how === 3) { a = this.startContainer; ao = this.startOffset; b = other.endContainer; bo = other.endOffset; }
    else throw dom("Failed to execute 'compareBoundaryPoints' on 'Range': The comparison method provided must be one of 'START_TO_START', 'START_TO_END', 'END_TO_END', or 'END_TO_START'.", 'NotSupportedError');
    return cmpPoints(a, ao, b, bo);
  });
  method(RP, 'comparePoint', function (node, offset) {
    checkOffset(node, offset, 'comparePoint');
    if (cmpPoints(node, offset, this.startContainer, this.startOffset) < 0) return -1;
    if (cmpPoints(node, offset, this.endContainer, this.endOffset) > 0) return 1;
    return 0;
  });
  method(RP, 'isPointInRange', function (node, offset) {
    if (rangeRoot(node) !== rangeRoot(this.startContainer)) return false;
    checkOffset(node, offset, 'isPointInRange');
    return cmpPoints(node, offset, this.startContainer, this.startOffset) >= 0 && cmpPoints(node, offset, this.endContainer, this.endOffset) <= 0;
  });
  method(RP, 'intersectsNode', function (node) {
    if (rangeRoot(node) !== rangeRoot(this.startContainer)) return false;
    var p = node.parentNode;
    if (!p) return true;
    var i = indexOf(node);
    return cmpPoints(p, i, this.endContainer, this.endOffset) < 0 && cmpPoints(p, i + 1, this.startContainer, this.startOffset) > 0;
  });
  function textOf(range) {
    var out = '', sc = range.startContainer, ec = range.endContainer;
    if (sc === ec && sc.nodeType === 3) return sc.data.slice(range.startOffset, range.endOffset);
    var root = range.commonAncestorContainer;
    var w = doc.createTreeWalker(root, 4);
    var n = root.nodeType === 3 ? root : w.nextNode();
    while (n) {
      if (cmpPoints(n, n.data.length, sc, range.startOffset) > 0 && cmpPoints(n, 0, ec, range.endOffset) < 0) {
        var from = n === sc ? range.startOffset : 0, to = n === ec ? range.endOffset : n.data.length;
        out += n.data.slice(from, to);
      }
      n = root.nodeType === 3 ? null : w.nextNode();
    }
    return out;
  }
  method(RP, 'toString', function () { return textOf(this); });
  // cloneContents / extractContents / deleteContents after the algorithm of the DOM standard
  function contents(range, mode) {
    var frag = doc.createDocumentFragment();
    if (range.collapsed) return frag;
    var sc = range.startContainer, so = range.startOffset, ec = range.endContainer, eo = range.endOffset;
    if (sc === ec && (sc.nodeType === 3 || sc.nodeType === 8)) {
      var clone = sc.cloneNode(false);
      clone.data = sc.data.slice(so, eo);
      frag.appendChild(clone);
      if (mode !== 'clone') sc.replaceData(so, eo - so, '');
      return frag;
    }
    var common = sc;
    while (!isInclusiveAncestor(common, ec)) common = common.parentNode;
    var firstPartial = null, lastPartial = null;
    if (!isInclusiveAncestor(sc, ec)) { firstPartial = sc; while (firstPartial.parentNode !== common) firstPartial = firstPartial.parentNode; }
    if (!isInclusiveAncestor(ec, sc)) { lastPartial = ec; while (lastPartial.parentNode !== common) lastPartial = lastPartial.parentNode; }
    var contained = [];
    for (var c = common.firstChild; c; c = c.nextSibling) {
      if (cmpPoints(c.parentNode, indexOf(c), sc, so) >= 0 && cmpPoints(c.parentNode, indexOf(c) + 1, ec, eo) <= 0) contained.push(c);
    }
    var newNode, newOffset;
    if (isInclusiveAncestor(sc, ec)) { newNode = sc; newOffset = so; }
    else { var ref = sc; while (ref.parentNode && !isInclusiveAncestor(ref.parentNode, ec)) ref = ref.parentNode; newNode = ref.parentNode; newOffset = indexOf(ref) + 1; }
    function sub(n1, o1, n2, o2) { var r = new Range(); r.startContainer = n1; r.startOffset = o1; r.endContainer = n2; r.endOffset = o2; refresh(r); return r; }
    if (firstPartial) {
      if (firstPartial.nodeType === 3 || firstPartial.nodeType === 8) {
        var cl = sc.cloneNode(false); cl.data = sc.data.slice(so); frag.appendChild(cl);
        if (mode !== 'clone') sc.replaceData(so, sc.data.length - so, '');
      } else {
        var cl2 = firstPartial.cloneNode(false); frag.appendChild(cl2);
        cl2.appendChild(contents(sub(sc, so, firstPartial, nodeLength(firstPartial)), mode));
      }
    }
    contained.forEach(function (n) {
      if (mode === 'clone') frag.appendChild(n.cloneNode(true));
      else frag.appendChild(n);
    });
    if (lastPartial) {
      if (lastPartial.nodeType === 3 || lastPartial.nodeType === 8) {
        var cl3 = ec.cloneNode(false); cl3.data = ec.data.slice(0, eo); frag.appendChild(cl3);
        if (mode !== 'clone') ec.replaceData(0, eo, '');
      } else {
        var cl4 = lastPartial.cloneNode(false); frag.appendChild(cl4);
        cl4.appendChild(contents(sub(lastPartial, 0, ec, eo), mode));
      }
    }
    if (mode !== 'clone') { range.startContainer = newNode; range.startOffset = newOffset; range.endContainer = newNode; range.endOffset = newOffset; refresh(range); }
    return frag;
  }
  method(RP, 'cloneContents', function () { return contents(this, 'clone'); });
  method(RP, 'extractContents', function () { return contents(this, 'extract'); });
  method(RP, 'deleteContents', function () { contents(this, 'extract'); });
  method(RP, 'insertNode', function (node) {
    var sc = this.startContainer, ref, parent;
    if (sc.nodeType === 3) { ref = sc.splitText(this.startOffset); parent = sc.parentNode; }
    else { ref = sc.childNodes[this.startOffset] || null; parent = sc; }
    if (node === ref) ref = ref.nextSibling;
    if (node.parentNode) node.parentNode.removeChild(node);
    var count = node.nodeType === 11 ? node.childNodes.length : 1;
    parent.insertBefore(node, ref);
    var idx = ref ? indexOf(ref) : parent.childNodes.length;
    if (this.collapsed) { this.endContainer = parent; this.endOffset = idx; refresh(this); }
    else if (this.endContainer === parent) { this.endOffset += count; refresh(this); }
  });
  method(RP, 'surroundContents', function (parent) {
    var frag = this.extractContents();
    while (parent.firstChild) parent.removeChild(parent.firstChild);
    this.insertNode(parent);
    parent.appendChild(frag);
    this.selectNode(parent);
  });
  method(RP, 'createContextualFragment', function (html) {
    var t = doc.createElement('template');
    t.innerHTML = html;
    return t.content;
  });
  method(RP, 'getBoundingClientRect', function () { return { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 }; });
  method(RP, 'getClientRects', function () { return []; });
  method(DP, 'createRange', function () { var r = new Range(); r.startContainer = doc; r.endContainer = doc; return r; });

  // ------------------------------------------------------------------------------------------------------------------ Selection
  function Selection() { throw new TypeError('Illegal constructor'); }
  hide(g, 'Selection', Selection);
  var SP = Selection.prototype;
  var selection = Object.create(SP);
  var current = null;
  function syncSelection() {
    var r = current;
    selection.rangeCount = r ? 1 : 0;
    selection.anchorNode = r ? r.startContainer : null; selection.anchorOffset = r ? r.startOffset : 0;
    selection.focusNode = r ? r.endContainer : null; selection.focusOffset = r ? r.endOffset : 0;
    selection.isCollapsed = r ? r.collapsed : true;
    selection.type = !r ? 'None' : r.collapsed ? 'Caret' : 'Range';
  }
  syncSelection();
  method(SP, 'getRangeAt', function (i) { if (!current || i !== 0) throw dom("Failed to execute 'getRangeAt' on 'Selection': " + i + ' is not a valid index.', 'IndexSizeError'); return current; });
  method(SP, 'addRange', function (r) { if (!current) { current = r; syncSelection(); } });
  method(SP, 'removeRange', function (r) { if (current === r) { current = null; syncSelection(); } });
  method(SP, 'removeAllRanges', function () { current = null; syncSelection(); });
  method(SP, 'empty', SP.removeAllRanges);
  method(SP, 'collapse', function (node, offset) {
    if (node === null) { current = null; syncSelection(); return; }
    var r = new Range(); r.setStart(node, offset || 0); r.collapse(true); current = r; syncSelection();
  });
  method(SP, 'setPosition', SP.collapse);
  method(SP, 'collapseToStart', function () { if (!current) throw dom("Failed to execute 'collapseToStart' on 'Selection': there is no selection.", 'InvalidStateError'); current.collapse(true); syncSelection(); });
  method(SP, 'collapseToEnd', function () { if (!current) throw dom("Failed to execute 'collapseToEnd' on 'Selection': there is no selection.", 'InvalidStateError'); current.collapse(false); syncSelection(); });
  method(SP, 'extend', function (node, offset) { if (!current) throw dom("Failed to execute 'extend' on 'Selection': This Selection object doesn't have any Ranges.", 'InvalidStateError'); current.setEnd(node, offset || 0); syncSelection(); });
  method(SP, 'setBaseAndExtent', function (an, ao, fn, fo) { var r = new Range(); r.setStart(an, ao); r.setEnd(fn, fo); current = r; syncSelection(); });
  method(SP, 'selectAllChildren', function (node) { var r = new Range(); r.selectNodeContents(node); current = r; syncSelection(); });
  method(SP, 'containsNode', function (node, partial) {
    if (!current) return false;
    var r = new Range(); r.selectNode(node);
    if (partial) return current.intersectsNode(node);
    return current.compareBoundaryPoints(0, r) <= 0 && current.compareBoundaryPoints(2, r) >= 0;
  });
  method(SP, 'deleteFromDocument', function () { if (current) { current.deleteContents(); syncSelection(); } });
  method(SP, 'toString', function () { return current ? current.toString() : ''; });
  method(SP, 'modify', function () {});
  hide(g, 'getSelection', function () { return selection; });
  method(DP, 'getSelection', function () { return selection; });

  // ------------------------------------------------------------------------------------------------------------------ DOMImplementation, XMLSerializer, Option
  function escapeText(t) { return t.split('&').join('&amp;').split('<').join('&lt;').split('>').join('&gt;'); }
  var implementation = {
    hasFeature: function () { return true; },
    createHTMLDocument: function (title) {
      var d = new DOMParser().parseFromString('<!DOCTYPE html><html><head>' + (title === undefined ? '' : '<title>' + escapeText(String(title)) + '</title>') + '</head><body></body></html>', 'text/html');
      return d;
    },
    createDocument: function (ns, name) { return new DOMParser().parseFromString(name ? '<' + name + '></' + name + '>' : '', 'application/xml'); },
    createDocumentType: function (name, publicId, systemId) { return { nodeType: 10, name: name, publicId: publicId, systemId: systemId }; }
  };
  try { if (doc.implementation === undefined) hide(doc, 'implementation', implementation); } catch (e) { }
  function XMLSerializer() {}
  hide(g, 'XMLSerializer', XMLSerializer);
  method(XMLSerializer.prototype, 'serializeToString', function (node) {
    if (!node) throw new TypeError("Failed to execute 'serializeToString' on 'XMLSerializer': parameter 1 is not of type 'Node'.");
    if (node.nodeType === 9) return node.documentElement ? node.documentElement.outerHTML : '';
    if (node.nodeType === 11) { var s = ''; for (var c = node.firstChild; c; c = c.nextSibling) s += this.serializeToString(c); return s; }
    if (node.nodeType === 3) return escapeText(String(node.data));
    if (node.nodeType === 8) return '<!--' + node.data + '-->';
    return node.outerHTML;
  });
  function Option(text, value, defaultSelected, selected) {
    var o = doc.createElement('option');
    if (text !== undefined) o.textContent = String(text);
    if (value !== undefined) o.setAttribute('value', String(value));
    if (defaultSelected) o.setAttribute('selected', '');
    if (selected !== undefined) o.selected = !!selected;
    return o;
  }
  Option.prototype = g.HTMLOptionElement.prototype;
  hide(g, 'Option', Option);
  function CompositionEvent(type, init) {
    var e = new UIEvent(type, init);
    e.data = init && init.data !== undefined ? String(init.data) : '';
    return e;
  }
  CompositionEvent.prototype = UIEvent.prototype;
  hide(g, 'CompositionEvent', CompositionEvent);

  // ------------------------------------------------------------------------------------------------------------------ interfaces of objects that exist already
  function alias(name, sample) {
    if (g[name] !== undefined || sample === null || sample === undefined) return;
    var f = illegal(name);
    f.prototype = Object.getPrototypeOf(sample);
    hide(g, name, f);
  }
  var probe = doc.createElement('div');
  alias('DOMTokenList', probe.classList);
  alias('CSSStyleDeclaration', probe.style);
  alias('History', g.history);
  alias('Location', g.location);
  alias('Navigator', g.navigator);
  alias('Storage', g.localStorage);

  // ------------------------------------------------------------------------------------------------------------------ small objects pages poke at
  if (g.screenX === undefined) { g.screenX = 0; g.screenY = 0; g.screenLeft = 0; g.screenTop = 0; }
  if (g.origin === undefined) g.origin = location.origin;
  if (g.crypto === undefined) {
    var HASHES = { 'SHA-1': 1, 'SHA-256': 2, 'SHA-384': 3, 'SHA-512': 4 };
    function hashKind(algorithm) {
      var name = typeof algorithm === 'string' ? algorithm : algorithm && (typeof algorithm.name === 'string' ? algorithm.name : null);
      var kind = name === null ? undefined : HASHES[name.toUpperCase()];
      if (!kind) throw new DOMException('Unrecognized name.', 'NotSupportedError');
      return kind;
    }
    function bytesOf(data) {
      if (data instanceof ArrayBuffer) return new Uint8Array(data);
      if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      throw new TypeError("Failed to execute on 'SubtleCrypto': parameter 2 is not of type '(ArrayBuffer or ArrayBufferView)'.");
    }
    function CryptoKey() { throw new TypeError('Illegal constructor'); }
    hide(g, 'CryptoKey', CryptoKey);
    function SubtleCrypto() { throw new TypeError('Illegal constructor'); }
    var subtle = Object.create(SubtleCrypto.prototype);
    hide(g, 'SubtleCrypto', SubtleCrypto);
    method(SubtleCrypto.prototype, 'digest', function (algorithm, data) {
      try { return Promise.resolve(__crypto_hash(hashKind(algorithm), bytesOf(data)).buffer); } catch (e) { return Promise.reject(e); }
    });
    method(SubtleCrypto.prototype, 'importKey', function (format, keyData, algorithm, extractable, usages) {
      try {
        if (format !== 'raw') throw new DOMException('Only raw keys are supported.', 'NotSupportedError');
        var name = typeof algorithm === 'string' ? algorithm : algorithm.name;
        if (String(name).toUpperCase() !== 'HMAC') throw new DOMException('Unsupported algorithm ' + name, 'NotSupportedError');
        var key = Object.create(CryptoKey.prototype);
        key.type = 'secret'; key.extractable = !!extractable; key.algorithm = { name: 'HMAC', hash: { name: typeof algorithm.hash === 'string' ? algorithm.hash : algorithm.hash.name }, length: bytesOf(keyData).length * 8 };
        key.usages = Array.from(usages || []);
        hide(key, '__bytes', new Uint8Array(bytesOf(keyData)));
        hide(key, '__hash', hashKind(algorithm.hash));
        return Promise.resolve(key);
      } catch (e) { return Promise.reject(e); }
    });
    method(SubtleCrypto.prototype, 'sign', function (algorithm, key, data) {
      try {
        if (!key || key.__bytes === undefined) throw new DOMException('The key is not usable for signing.', 'InvalidAccessError');
        return Promise.resolve(__crypto_hmac(key.__hash, key.__bytes, bytesOf(data)).buffer);
      } catch (e) { return Promise.reject(e); }
    });
    method(SubtleCrypto.prototype, 'verify', function (algorithm, key, signature, data) {
      return this.sign(algorithm, key, data).then(function (mac) {
        var a = new Uint8Array(mac), b = bytesOf(signature), same = a.length === b.length;
        for (var i = 0; i < a.length && same; i++) same = a[i] === b[i];
        return same;
      });
    });
    method(SubtleCrypto.prototype, 'exportKey', function (format, key) {
      if (!key || key.__bytes === undefined || !key.extractable) return Promise.reject(new DOMException('The key is not extractable.', 'InvalidAccessError'));
      return Promise.resolve(new Uint8Array(key.__bytes).buffer);
    });
    hide(g, 'crypto', {
      subtle: subtle,
      getRandomValues: function (array) {
        if (!ArrayBuffer.isView(array) || array instanceof Float32Array || array instanceof Float64Array) throw new DOMException("The data argument must be an integer-type TypedArray.", 'TypeMismatchError');
        if (array.byteLength > 65536) throw new DOMException('The ArrayBufferView\'s byte length (' + array.byteLength + ') exceeds the number of bytes of entropy available via this API (65536).', 'QuotaExceededError');
        var max = array.BYTES_PER_ELEMENT === 4 ? 4294967296 : array.BYTES_PER_ELEMENT === 2 ? 65536 : 256;
        for (var i = 0; i < array.length; i++) array[i] = Math.floor(Math.random() * max);
        return array;
      },
      randomUUID: function () {
        var template = 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx', out = '';
        for (var i = 0; i < template.length; i++) {
          var c = template.charAt(i);
          if (c !== 'x' && c !== 'y') { out += c; continue; }
          var r = Math.floor(Math.random() * 16);
          out += (c === 'x' ? r : (r & 3) | 8).toString(16);
        }
        return out;
      }
    });
  }
  var resolved = function (v) { return Promise.resolve(v); };
  method(EP, 'animate', function (keyframes, options) {
    var anim = { currentTime: 0, playState: 'finished', pending: false, finished: resolved(null), ready: resolved(null), onfinish: null, oncancel: null,
                 play: function () {}, pause: function () {}, cancel: function () { if (typeof anim.oncancel === 'function') anim.oncancel(); }, finish: function () {}, reverse: function () {},
                 addEventListener: function () {}, removeEventListener: function () {} };
    setTimeout(function () { if (typeof anim.onfinish === 'function') anim.onfinish({ type: 'finish', target: anim }); }, 0);
    return anim;
  });
  method(EP, 'getAnimations', function () { return []; });
  method(EP, 'setPointerCapture', function () {});
  method(EP, 'releasePointerCapture', function () {});
  method(EP, 'hasPointerCapture', function () { return false; });
  method(EP, 'requestPointerLock', function () {});
  // --- Fullscreen API. The window hides its own controls and the element covers the viewport (the :fullscreen rule of the page); promises and events are here.
  var fsFire = function (target, type) { target.dispatchEvent(new Event(type, { bubbles: true, composed: true })); };
  var fsError = function (el) {
    setTimeout(function () { fsFire(el, 'fullscreenerror'); }, 0);
    return Promise.reject(new TypeError("Failed to execute 'requestFullscreen' on 'Element': API can only be initiated by a user gesture or the element cannot be fullscreen."));
  };
  var fsRequest = function (options) {
    var el = this;
    if (options !== undefined && options !== null && typeof options !== 'object') return Promise.reject(new TypeError("Failed to execute 'requestFullscreen' on 'Element': The provided value is not of type 'FullscreenOptions'."));
    if (!el.isConnected || !__fs_supported()) return fsError(el);
    var already = __fs_element() === el;
    if (!__fs_request(el)) return fsError(el);
    return new Promise(function (resolve) {
      setTimeout(function () { if (!already) fsFire(el, 'fullscreenchange'); resolve(); }, 0);
    });
  };
  method(EP, 'requestFullscreen', fsRequest);
  method(EP, 'webkitRequestFullscreen', fsRequest);
  method(EP, 'webkitRequestFullScreen', fsRequest);
  var fsExit = function () {
    var el = __fs_element();
    if (!el) return Promise.reject(new TypeError("Failed to execute 'exitFullscreen' on 'Document': Document not active"));
    __fs_exit();
    return new Promise(function (resolve) { setTimeout(function () { fsFire(el, 'fullscreenchange'); resolve(); }, 0); });
  };
  method(DP, 'exitFullscreen', fsExit);
  method(DP, 'webkitExitFullscreen', fsExit);
  method(DP, 'webkitCancelFullScreen', fsExit);
  var fsAccessor = function (name, get) { Object.defineProperty(DP, name, { get: get, configurable: true, enumerable: true }); };
  fsAccessor('fullscreenElement', function () { return __fs_element(); });
  fsAccessor('webkitFullscreenElement', function () { return __fs_element(); });
  fsAccessor('webkitCurrentFullScreenElement', function () { return __fs_element(); });
  fsAccessor('fullscreenEnabled', function () { return __fs_supported(); });
  fsAccessor('webkitFullscreenEnabled', function () { return __fs_supported(); });
  fsAccessor('fullscreen', function () { return __fs_element() !== null; });
  fsAccessor('webkitIsFullScreen', function () { return __fs_element() !== null; });
  if (g.HTMLVideoElement) {
    method(g.HTMLVideoElement.prototype, 'webkitEnterFullscreen', function () { return fsRequest.call(this); });
    method(g.HTMLVideoElement.prototype, 'webkitExitFullscreen', function () { return fsExit(); });
    Object.defineProperty(g.HTMLVideoElement.prototype, 'webkitDisplayingFullscreen', { get: function () { return __fs_element() === this; }, configurable: true, enumerable: true });
  }
  // The window left fullscreen by itself (Escape): the events follow, as for exitFullscreen().
  hide(g, '__fs_left', function () { setTimeout(function () { fsFire(g.document, 'fullscreenchange'); }, 0); });
  method(DP, 'elementsFromPoint', function () { return []; });
  method(DP, 'execCommand', function () { return false; });
  method(DP, 'queryCommandSupported', function () { return false; });
  method(DP, 'queryCommandEnabled', function () { return false; });
  method(DP, 'queryCommandState', function () { return false; });
  method(DP, 'startViewTransition', function (callback) {
    var done = Promise.resolve().then(function () { return typeof callback === 'function' ? callback() : undefined; });
    return { ready: done, finished: done, updateCallbackDone: done, skipTransition: function () {} };
  });
  if (doc.fonts === undefined) {
    hide(doc, 'fonts', { ready: resolved(null), status: 'loaded', size: 0, check: function () { return true; }, load: function () { return resolved([]); },
                         add: function () {}, delete: function () { return false; }, clear: function () {}, forEach: function () {}, has: function () { return false; },
                         addEventListener: function () {}, removeEventListener: function () {} });
  }
  if (doc.timeline === undefined) hide(doc, 'timeline', { currentTime: 0 });
  if (doc.designMode === undefined) doc.designMode = 'off';
  if (doc.dir === undefined) doc.dir = '';

  // ------------------------------------------------------------------------------------------------------------------ MutationObserver
  var observers = [];   // every observer that observes something
  function MutationObserver(callback) {
    if (!(this instanceof MutationObserver)) throw new TypeError("Failed to construct 'MutationObserver': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (typeof callback !== 'function') throw new TypeError("Failed to construct 'MutationObserver': The callback provided as parameter 1 is not a function.");
    hide(this, '__callback', callback);
    hide(this, '__targets', []);
    hide(this, '__queue', []);
  }
  method(MutationObserver.prototype, 'observe', function (target, options) {
    if (!target || typeof target.nodeType !== 'number') throw new TypeError("Failed to execute 'observe' on 'MutationObserver': parameter 1 is not of type 'Node'.");
    options = options || {};
    var o = { childList: !!options.childList, subtree: !!options.subtree, attributes: options.attributes, characterData: options.characterData,
              attributeOldValue: !!options.attributeOldValue, characterDataOldValue: !!options.characterDataOldValue, attributeFilter: options.attributeFilter };
    if (o.attributes === undefined && (o.attributeOldValue || o.attributeFilter !== undefined)) o.attributes = true;
    if (o.characterData === undefined && o.characterDataOldValue) o.characterData = true;
    o.attributes = !!o.attributes; o.characterData = !!o.characterData;
    if (!o.childList && !o.attributes && !o.characterData)
      throw new TypeError("Failed to execute 'observe' on 'MutationObserver': The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.");
    if (o.attributeOldValue && !o.attributes) throw new TypeError("Failed to execute 'observe' on 'MutationObserver': The options object may only set 'attributeOldValue' to true when 'attributes' is true or not present.");
    for (var i = 0; i < this.__targets.length; i++) if (this.__targets[i].node === target) { this.__targets[i].options = o; return; }
    this.__targets.push({ node: target, options: o });
    if (observers.indexOf(this) < 0) observers.push(this);
    __mutations_on();
    __watch_node(target);
  });
  method(MutationObserver.prototype, 'disconnect', function () {
    this.__targets.length = 0;
    this.__queue.length = 0;
    var at = observers.indexOf(this);
    if (at >= 0) observers.splice(at, 1);
  });
  method(MutationObserver.prototype, 'takeRecords', function () { processLog(); var q = this.__queue.slice(); this.__queue.length = 0; return q; });
  hide(g, 'MutationObserver', MutationObserver);
  function MutationRecord() { throw new TypeError('Illegal constructor'); }
  hide(g, 'MutationRecord', MutationRecord);

  function reportError(e) {
    try { setTimeout(function () { throw e; }, 0); } catch (ignored) { }
  }
  function deliver(record) {
    for (var i = 0; i < observers.length; i++) {
      var observer = observers[i];
      for (var t = 0; t < observer.__targets.length; t++) {
        var reg = observer.__targets[t], o = reg.options;
        var applies = reg.node === record.target || (o.subtree && record.watchers.indexOf(reg.node) >= 0);
        if (!applies) continue;
        if (record.type === 'childList' && !o.childList) continue;
        if (record.type === 'attributes' && (!o.attributes || (o.attributeFilter && o.attributeFilter.indexOf(record.attributeName) < 0))) continue;
        if (record.type === 'characterData' && !o.characterData) continue;
        var copy = Object.create(MutationRecord.prototype);
        copy.type = record.type; copy.target = record.target; copy.addedNodes = record.addedNodes; copy.removedNodes = record.removedNodes;
        copy.previousSibling = record.previousSibling; copy.nextSibling = record.nextSibling; copy.attributeName = record.attributeName;
        copy.attributeNamespace = null;
        copy.oldValue = (record.type === 'attributes' && o.attributeOldValue) || (record.type === 'characterData' && o.characterDataOldValue) ? record.oldValue : null;
        observer.__queue.push(copy);
        break;   // (an observer gets a record once)
      }
    }
  }

  // ------------------------------------------------------------------------------------------------------------------ custom elements
  var definitions = {}, waiting = {};
  function validName(name) {
    if (name.indexOf('-') < 1 || name !== name.toLowerCase() || !/^[a-z]/.test(name)) return false;
    return ['annotation-xml', 'color-profile', 'font-face', 'font-face-src', 'font-face-uri', 'font-face-format', 'font-face-name', 'missing-glyph'].indexOf(name) < 0;
  }
  function upgrade(el) {
    var def = definitions[el.localName];
    if (!def || el.__ce !== undefined) return;
    Object.defineProperty(el, '__ce', { value: 1, writable: true });
    Object.setPrototypeOf(el, def.ctor.prototype);
    try { def.ctor.call(el); } catch (e) { reportError(e); }
    for (var i = 0; i < def.observed.length; i++) {
      var a = def.observed[i];
      if (el.hasAttribute(a)) callback(el, 'attributeChangedCallback', [a, null, el.getAttribute(a)]);
    }
    if (el.isConnected) callback(el, 'connectedCallback', []);
  }
  function callback(el, name, args) {
    var fn = el[name];
    if (typeof fn !== 'function') return;
    try { fn.apply(el, args); } catch (e) { reportError(e); }
  }
  function eachElement(node, fn) {
    if (node.nodeType === 1) fn(node);
    for (var c = node.firstChild; c; c = c.nextSibling) eachElement(c, fn);
  }
  function reactions(record) {
    if (record.type === 'childList') {
      for (var i = 0; i < record.addedNodes.length; i++) eachElement(record.addedNodes[i], function (el) {
        if (definitions[el.localName] && el.__ce === undefined && el.isConnected) upgrade(el);
        else if (el.__ce !== undefined && el.isConnected) callback(el, 'connectedCallback', []);
      });
      for (var j = 0; j < record.removedNodes.length; j++) eachElement(record.removedNodes[j], function (el) {
        if (el.__ce !== undefined && !el.isConnected) callback(el, 'disconnectedCallback', []);
      });
    } else if (record.type === 'attributes') {
      var el = record.target, def = definitions[el.localName];
      if (def && el.__ce !== undefined && def.observed.indexOf(record.attributeName) >= 0)
        callback(el, 'attributeChangedCallback', [record.attributeName, record.oldValue, el.getAttribute(record.attributeName)]);
    }
  }
  // The changes since the last time: custom element reactions run, observers get their records queued.
  function processLog() {
    var records = __take_mutations();
    for (var i = 0; i < records.length; i++) {
      var r = records[i];
      if (r.target.nodeType === 1 || r.type === 'childList') reactions(r);
      deliver(r);
    }
  }
  hide(g, '__process_mutations', processLog);
  hide(g, '__deliver_mutations', function () {
    processLog();
    for (var o = 0; o < observers.length; o++) {
      var observer = observers[o];
      if (observer.__queue.length === 0) continue;
      var queue = observer.__queue.slice();
      observer.__queue.length = 0;
      try { observer.__callback.call(observer, queue, observer); } catch (e) { reportError(e); }
    }
  });
  function CustomElementRegistry() { throw new TypeError('Illegal constructor'); }
  var registry = Object.create(CustomElementRegistry.prototype);
  method(CustomElementRegistry.prototype, 'define', function (name, ctor, options) {
    name = String(name);
    if (typeof ctor !== 'function') throw new TypeError("Failed to execute 'define' on 'CustomElementRegistry': The callback provided as parameter 2 is not a function.");
    if (!validName(name)) throw new DOMException("Failed to execute 'define' on 'CustomElementRegistry': \"" + name + '" is not a valid custom element name', 'SyntaxError');
    if (definitions[name]) throw new DOMException("Failed to execute 'define' on 'CustomElementRegistry': the name \"" + name + '" has already been used with this registry', 'NotSupportedError');
    for (var other in definitions) if (definitions[other].ctor === ctor) throw new DOMException("Failed to execute 'define' on 'CustomElementRegistry': this constructor has already been used with this registry", 'NotSupportedError');
    var observed = [];
    var list = ctor.observedAttributes;
    if (list) for (var i = 0; i < list.length; i++) observed.push(String(list[i]));
    definitions[name] = { ctor: ctor, observed: observed };
    __register_tag(name, ctor.prototype);
    __mutations_on();
    var found = doc.querySelectorAll(name);
    for (var k = 0; k < found.length; k++) upgrade(found[k]);
    if (waiting[name]) { waiting[name].forEach(function (resolve) { resolve(ctor); }); delete waiting[name]; }
  });
  method(CustomElementRegistry.prototype, 'get', function (name) { var d = definitions[String(name)]; return d ? d.ctor : undefined; });
  method(CustomElementRegistry.prototype, 'getName', function (ctor) { for (var n in definitions) if (definitions[n].ctor === ctor) return n; return null; });
  method(CustomElementRegistry.prototype, 'whenDefined', function (name) {
    name = String(name);
    if (!validName(name)) return Promise.reject(new DOMException("Failed to execute 'whenDefined' on 'CustomElementRegistry': \"" + name + '" is not a valid custom element name', 'SyntaxError'));
    if (definitions[name]) return Promise.resolve(definitions[name].ctor);
    return new Promise(function (resolve) { (waiting[name] = waiting[name] || []).push(resolve); });
  });
  method(CustomElementRegistry.prototype, 'upgrade', function (root) { eachElement(root, function (el) { if (definitions[el.localName]) upgrade(el); }); });
  hide(g, 'CustomElementRegistry', CustomElementRegistry);
  hide(g, 'customElements', registry);
  // ------------------------------------------------------------------------------------------------------------------ IndexedDB
  // The databases of the origin are kept as text in localStorage (so that pages of the origin share them), worked on as objects while a page uses them. A
  // transaction is a queue of requests that are answered one after the other, each as a task of its own; it completes when the queue is empty.
  if (g.indexedDB === undefined) (function () {
    var def = hide;
    var PREFIX = '__idb__';
    var open = {};   // name -> the database as it is in memory
    function store() { try { return g.localStorage; } catch (e) { return null; } }
    function load(name) {
      var ls = store(), text = ls ? ls.getItem(PREFIX + name) : null;
      if (text === null) return null;
      try { return __wire_decode(text); } catch (e) { return null; }
    }
    function save(db) { var ls = store(); if (ls) try { ls.setItem(PREFIX + db.name, __wire_encode(db)); } catch (e) { } }
    function dataError(message) { return new DOMException(message, 'DataError'); }
    function typeOrder(k) { return Array.isArray(k) ? 5 : typeof k === 'string' ? 4 : k instanceof Date ? 2 : typeof k === 'number' ? 1 : ArrayBuffer.isView(k) || k instanceof ArrayBuffer ? 3 : 0; }
    function valid(k) {
      if (typeof k === 'number') return k === k;
      if (typeof k === 'string' || k instanceof Date) return !(k instanceof Date) || k.getTime() === k.getTime();
      if (Array.isArray(k)) return k.every(valid);
      return k instanceof ArrayBuffer || ArrayBuffer.isView(k);
    }
    function compare(a, b) {
      var ta = typeOrder(a), tb = typeOrder(b);
      if (ta !== tb) return ta < tb ? -1 : 1;
      if (ta === 1) return a < b ? -1 : a > b ? 1 : 0;
      if (ta === 2) return a.getTime() < b.getTime() ? -1 : a.getTime() > b.getTime() ? 1 : 0;
      if (ta === 4) return a < b ? -1 : a > b ? 1 : 0;
      if (ta === 3) { var x = new Uint8Array(a.buffer || a), y = new Uint8Array(b.buffer || b); for (var i = 0; i < M.min(x.length, y.length); i++) if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1; return x.length < y.length ? -1 : x.length > y.length ? 1 : 0; }
      for (var j = 0; j < M.min(a.length, b.length); j++) { var c = compare(a[j], b[j]); if (c) return c; }
      return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
    }
    var M = Math;
    function keyAt(path, value) {
      if (Array.isArray(path)) { var out = []; for (var i = 0; i < path.length; i++) { var k = keyAt(path[i], value); if (k === undefined) return undefined; out.push(k); } return out; }
      if (path === '') return value;
      var cur = value, parts = String(path).split('.');
      for (var p = 0; p < parts.length; p++) { if (cur === null || cur === undefined) return undefined; cur = cur[parts[p]]; }
      return cur;
    }
    function setKeyAt(path, value, key) {
      var parts = String(path).split('.'), cur = value;
      for (var i = 0; i < parts.length - 1; i++) { if (cur[parts[i]] === undefined) cur[parts[i]] = {}; cur = cur[parts[i]]; }
      cur[parts[parts.length - 1]] = key;
    }
    function find(records, key) {   // [index, found]
      var lo = 0, hi = records.length;
      while (lo < hi) { var mid = (lo + hi) >> 1, c = compare(records[mid].key, key); if (c < 0) lo = mid + 1; else hi = mid; }
      return [lo, lo < records.length && compare(records[lo].key, key) === 0];
    }
    function clone(v) { return __wire_decode(__wire_encode(v)); }

    function IDBKeyRange() { throw new TypeError('Illegal constructor'); }
    function range(lower, upper, lowerOpen, upperOpen) { var r = Object.create(IDBKeyRange.prototype); r.lower = lower; r.upper = upper; r.lowerOpen = !!lowerOpen; r.upperOpen = !!upperOpen; return r; }
    def(IDBKeyRange, 'only', function (v) { if (!valid(v)) throw dataError('The parameter is not a valid key.'); return range(v, v, false, false); });
    def(IDBKeyRange, 'lowerBound', function (v, open) { if (!valid(v)) throw dataError('The parameter is not a valid key.'); return range(v, undefined, open, true); });
    def(IDBKeyRange, 'upperBound', function (v, open) { if (!valid(v)) throw dataError('The parameter is not a valid key.'); return range(undefined, v, true, open); });
    def(IDBKeyRange, 'bound', function (l, u, lo, uo) {
      if (!valid(l) || !valid(u)) throw dataError('The parameter is not a valid key.');
      if (compare(l, u) > 0 || (compare(l, u) === 0 && (lo || uo))) throw dataError('The lower key is greater than the upper key.');
      return range(l, u, lo, uo);
    });
    def(IDBKeyRange.prototype, 'includes', function (k) {
      if (this.lower !== undefined) { var c = compare(k, this.lower); if (c < 0 || (c === 0 && this.lowerOpen)) return false; }
      if (this.upper !== undefined) { var d = compare(k, this.upper); if (d > 0 || (d === 0 && this.upperOpen)) return false; }
      return true;
    });
    def(g, 'IDBKeyRange', IDBKeyRange);
    function toRange(query) {
      if (query === undefined || query === null) return null;
      if (query instanceof IDBKeyRange) return query;
      if (!valid(query)) throw dataError('The parameter is not a valid key.');
      return range(query, query, false, false);
    }

    function IDBRequest() { throw new TypeError('Illegal constructor'); }
    IDBRequest.prototype = Object.create(EventTarget.prototype);
    def(IDBRequest.prototype, 'constructor', IDBRequest);
    def(g, 'IDBRequest', IDBRequest);
    function IDBOpenDBRequest() { throw new TypeError('Illegal constructor'); }
    IDBOpenDBRequest.prototype = Object.create(IDBRequest.prototype);
    def(g, 'IDBOpenDBRequest', IDBOpenDBRequest);
    function newRequest(source, transaction, proto) {
      var r = Object.create(proto || IDBRequest.prototype);
      r.result = undefined; r.error = null; r.source = source; r.transaction = transaction; r.readyState = 'pending';
      r.onsuccess = null; r.onerror = null;
      return r;
    }
    function fire(target, type, init) {
      var e = new Event(type, init || {});
      target.dispatchEvent(e);
      return e;
    }

    function IDBTransaction() { throw new TypeError('Illegal constructor'); }
    IDBTransaction.prototype = Object.create(EventTarget.prototype);
    def(IDBTransaction.prototype, 'constructor', IDBTransaction);
    def(g, 'IDBTransaction', IDBTransaction);
    function newTransaction(connection, names, mode) {
      var t = Object.create(IDBTransaction.prototype);
      t.db = connection; t.mode = mode; t.error = null; t.durability = 'default'; t.objectStoreNames = list(names);
      t.oncomplete = null; t.onabort = null; t.onerror = null;
      Object.defineProperty(t, '__queue', { value: [], writable: true });
      Object.defineProperty(t, '__state', { value: { active: true, finished: false, running: false, wrote: false } });
      Object.defineProperty(t, '__stores', { value: names });
      return t;
    }
    function list(names) { var l = names.slice(); l.contains = function (n) { return names.indexOf(n) >= 0; }; l.item = function (i) { return names[i] === undefined ? null : names[i]; }; return l; }
    function schedule(t) {
      if (t.__state.running || t.__state.finished) return;
      t.__state.running = true;
      setTimeout(function () { t.__state.running = false; next(t); }, 0);
    }
    function next(t) {
      if (t.__state.finished) return;
      var job = t.__queue.shift();
      if (!job) { finish(t); return; }
      var request = job.request;
      try {
        request.result = job.run();
        request.readyState = 'done';
        t.__state.active = true;
        var e = fire(request, 'success');
      } catch (error) {
        request.readyState = 'done';
        request.error = error;
        t.__state.active = true;
        var f = new Event('error', { bubbles: true, cancelable: true });
        request.dispatchEvent(f);
        if (!f.defaultPrevented) { t.error = error; abort(t, error); return; }
      }
      schedule(t);
    }
    function finish(t) {
      if (t.__state.finished) return;
      t.__state.finished = true; t.__state.active = false;
      if (t.__state.wrote) save(t.db.__data);
      fire(t, 'complete');
      t.db.__activeTransactions--;
    }
    function abort(t, error) {
      if (t.__state.finished) return;
      t.__state.finished = true; t.__state.active = false;
      if (t.__restore) t.__restore();
      t.error = error || null;
      t.__queue.length = 0;
      fire(t, 'abort');
      t.db.__activeTransactions--;
    }
    def(IDBTransaction.prototype, 'abort', function () { if (this.__state.finished) throw new DOMException('The transaction has already completed or aborted.', 'InvalidStateError'); abort(this, null); });
    def(IDBTransaction.prototype, 'commit', function () {});
    def(IDBTransaction.prototype, 'objectStore', function (name) {
      if (this.__state.finished) throw new DOMException("Failed to execute 'objectStore' on 'IDBTransaction': The transaction has finished.", 'InvalidStateError');
      if (this.__stores.indexOf(name) < 0 || !this.db.__data.stores[name]) throw new DOMException("Failed to execute 'objectStore' on 'IDBTransaction': The specified object store was not found.", 'NotFoundError');
      return newObjectStore(this, name);
    });
    function enqueue(t, source, run, write) {
      if (t.__state.finished) throw new DOMException('The transaction has finished.', 'TransactionInactiveError');
      if (write && t.mode === 'readonly') throw new DOMException('The transaction is read-only.', 'ReadOnlyError');
      var request = newRequest(source, t);
      if (write) t.__state.wrote = true;
      t.__queue.push({ request: request, run: run });
      schedule(t);
      return request;
    }

    function IDBObjectStore() { throw new TypeError('Illegal constructor'); }
    function IDBIndex() { throw new TypeError('Illegal constructor'); }
    function IDBCursor() { throw new TypeError('Illegal constructor'); }
    function IDBCursorWithValue() { throw new TypeError('Illegal constructor'); }
    IDBCursorWithValue.prototype = Object.create(IDBCursor.prototype);
    def(g, 'IDBObjectStore', IDBObjectStore); def(g, 'IDBIndex', IDBIndex); def(g, 'IDBCursor', IDBCursor); def(g, 'IDBCursorWithValue', IDBCursorWithValue);
    function newObjectStore(t, name) {
      var s = Object.create(IDBObjectStore.prototype), data = t.db.__data.stores[name];
      s.name = name; s.keyPath = data.keyPath; s.autoIncrement = data.autoIncrement; s.transaction = t; s.indexNames = list(Object.keys(data.indexes));
      Object.defineProperty(s, '__data', { value: data });
      return s;
    }
    function recordsOf(source) { return source.__index ? source.__index : null; }
    function indexRows(store, indexName) {   // [{ key: indexKey, primaryKey, value }] sorted
      var def_ = store.__data.indexes[indexName], rows = [];
      store.__data.records.forEach(function (rec) {
        var k = keyAt(def_.keyPath, rec.value);
        if (k === undefined) return;
        var keys = def_.multiEntry && Array.isArray(k) ? k.filter(valid) : (valid(k) ? [k] : []);
        keys.forEach(function (ik) { rows.push({ key: ik, primaryKey: rec.key, value: rec.value }); });
      });
      rows.sort(function (a, b) { return compare(a.key, b.key) || compare(a.primaryKey, b.primaryKey); });
      return rows;
    }
    function checkKey(store, value, key, adding) {
      var data = store.__data, result = key;
      if (data.keyPath !== null) {
        if (key !== undefined) throw dataError('The object store uses in-line keys and the key parameter was provided.');
        result = keyAt(data.keyPath, value);
        if (result === undefined) {
          if (!data.autoIncrement) throw dataError('Evaluating the object store\'s key path did not yield a value.');
          result = data.nextKey++;
          if (typeof value === 'object' && value !== null && !Array.isArray(data.keyPath)) setKeyAt(data.keyPath, value, result);
        } else if (typeof result === 'number' && data.autoIncrement && result >= data.nextKey) data.nextKey = M.floor(result) + 1;
      } else if (result === undefined) {
        if (!data.autoIncrement) throw dataError('The object store uses out-of-line keys and has no key generator and the key parameter was not provided.');
        result = data.nextKey++;
      } else if (typeof result === 'number' && data.autoIncrement && result >= data.nextKey) data.nextKey = M.floor(result) + 1;
      if (!valid(result)) throw dataError('The parameter is not a valid key.');
      return result;
    }
    function put(store, value, key, adding) {
      var t = store.transaction, copy = clone(value), data = store.__data;
      var k = checkKey(store, copy, key, adding);
      return enqueue(t, store, function () {
        var at = find(data.records, k);
        if (at[1] && adding) throw new DOMException('Unable to add key: a record with the key already exists.', 'ConstraintError');
        // unique indexes
        Object.keys(data.indexes).forEach(function (n) {
          var d = data.indexes[n]; if (!d.unique) return;
          var ik = keyAt(d.keyPath, copy);
          if (ik === undefined) return;
          data.records.forEach(function (rec) { if (compare(rec.key, k) !== 0 && compare(keyAt(d.keyPath, rec.value), ik) === 0) throw new DOMException('Unable to add key: a record with the index key already exists.', 'ConstraintError'); });
        });
        if (at[1]) data.records[at[0]] = { key: k, value: copy }; else data.records.splice(at[0], 0, { key: k, value: copy });
        return k;
      }, true);
    }
    var SP_ = IDBObjectStore.prototype;
    def(SP_, 'add', function (value, key) { return put(this, value, key, true); });
    def(SP_, 'put', function (value, key) { return put(this, value, key, false); });
    def(SP_, 'get', function (query) {
      var data = this.__data, r = toRange(query);
      if (r === null) throw dataError('The parameter is not a valid key.');
      return enqueue(this.transaction, this, function () { for (var i = 0; i < data.records.length; i++) if (r.includes(data.records[i].key)) return clone(data.records[i].value); return undefined; });
    });
    def(SP_, 'getKey', function (query) {
      var data = this.__data, r = toRange(query);
      if (r === null) throw dataError('The parameter is not a valid key.');
      return enqueue(this.transaction, this, function () { for (var i = 0; i < data.records.length; i++) if (r.includes(data.records[i].key)) return data.records[i].key; return undefined; });
    });
    function gather(store, query, count, wantKeys) {
      var data = store.__data, r = toRange(query);
      return enqueue(store.transaction, store, function () {
        var out = [];
        for (var i = 0; i < data.records.length && (count === undefined || out.length < count); i++) if (!r || r.includes(data.records[i].key)) out.push(wantKeys ? data.records[i].key : clone(data.records[i].value));
        return out;
      });
    }
    def(SP_, 'getAll', function (query, count) { return gather(this, query, count, false); });
    def(SP_, 'getAllKeys', function (query, count) { return gather(this, query, count, true); });
    def(SP_, 'count', function (query) { var data = this.__data, r = toRange(query); return enqueue(this.transaction, this, function () { var n = 0; data.records.forEach(function (rec) { if (!r || r.includes(rec.key)) n++; }); return n; }); });
    def(SP_, 'delete', function (query) {
      var data = this.__data, r = toRange(query);
      if (r === null) throw dataError('The parameter is not a valid key.');
      return enqueue(this.transaction, this, function () { data.records = data.records.filter(function (rec) { return !r.includes(rec.key); }); return undefined; }, true);
    });
    def(SP_, 'clear', function () { var data = this.__data; return enqueue(this.transaction, this, function () { data.records = []; return undefined; }, true); });
    def(SP_, 'index', function (name) {
      if (!this.__data.indexes[name]) throw new DOMException("Failed to execute 'index' on 'IDBObjectStore': The specified index was not found.", 'NotFoundError');
      var i = Object.create(IDBIndex.prototype), d = this.__data.indexes[name];
      i.name = name; i.objectStore = this; i.keyPath = d.keyPath; i.unique = d.unique; i.multiEntry = d.multiEntry;
      return i;
    });
    def(SP_, 'createIndex', function (name, keyPath, options) {
      if (this.transaction.mode !== 'versionchange') throw new DOMException("Failed to execute 'createIndex' on 'IDBObjectStore': The database is not running a version change transaction.", 'InvalidStateError');
      if (this.__data.indexes[name]) throw new DOMException("Failed to execute 'createIndex' on 'IDBObjectStore': An index with the specified name already exists.", 'ConstraintError');
      this.__data.indexes[name] = { keyPath: keyPath, unique: !!(options && options.unique), multiEntry: !!(options && options.multiEntry) };
      this.transaction.__state.wrote = true;
      return this.index(name);
    });
    def(SP_, 'deleteIndex', function (name) { delete this.__data.indexes[name]; this.transaction.__state.wrote = true; });
    function cursorFor(source, rows, query, direction, withValue, update) {
      var r = toRange(query), t = source.transaction || source.objectStore.transaction;
      direction = direction || 'next';
      var request = null;
      request = enqueue(t, source, function () {
        var list_ = rows().filter(function (row) { return !r || r.includes(row.key); });
        if (direction === 'prev' || direction === 'prevunique') list_.reverse();
        if (direction === 'nextunique' || direction === 'prevunique') list_ = list_.filter(function (row, i) { return i === 0 || compare(row.key, list_[i - 1].key) !== 0; });
        if (list_.length === 0) return null;
        var position = 0;
        var cursor = Object.create(withValue ? IDBCursorWithValue.prototype : IDBCursor.prototype);
        function show() { var row = list_[position]; cursor.key = row.key; cursor.primaryKey = row.primaryKey; if (withValue) cursor.value = clone(row.value); }
        cursor.source = source; cursor.direction = direction; show();
        def(cursor, '__req', request);
        def(cursor, '__move', function (steps, key) {
          if (!this.__req) throw new DOMException('The cursor is not active.', 'InvalidStateError');
          var req = this.__req;
          req.readyState = 'pending';
          t.__queue.push({ request: req, run: function () {
            position += steps;
            if (key !== undefined) while (position < list_.length && compare(list_[position].key, key) < 0) position++;
            if (position >= list_.length) return null;
            show();
            return cursor;
          } });
          schedule(t);
        });
        def(cursor, 'continue', function (key) { this.__move(1, key); });
        def(cursor, 'advance', function (n) { if (n < 1) throw new TypeError("Failed to execute 'advance' on 'IDBCursor': A count argument with value 0 (zero) was supplied, must be greater than 0."); this.__move(n); });
        def(cursor, 'update', function (value) {
          if (source.__data === undefined && source.objectStore === undefined) throw new DOMException('Cannot update.', 'InvalidStateError');
          var store = source.__data ? source : source.objectStore;
          return put(store, value, store.__data.keyPath === null ? cursor.primaryKey : undefined, false);
        });
        def(cursor, 'delete', function () { var store = source.__data ? source : source.objectStore; return store.delete(cursor.primaryKey); });
        return cursor;
      });
      return request;
    }
    function bindRequest(request, cursor) { return request; }
    def(SP_, 'openCursor', function (query, direction) {
      var self = this, req = cursorFor(this, function () { return self.__data.records.map(function (rec) { return { key: rec.key, primaryKey: rec.key, value: rec.value }; }); }, query, direction, true);
      attachCursorRequest(req); return req;
    });
    def(SP_, 'openKeyCursor', function (query, direction) {
      var self = this, req = cursorFor(this, function () { return self.__data.records.map(function (rec) { return { key: rec.key, primaryKey: rec.key, value: rec.value }; }); }, query, direction, false);
      attachCursorRequest(req); return req;
    });
    // the request of a cursor is answered again for every step: the cursor remembers it
    function attachCursorRequest(req) {
      Object.defineProperty(req, '__cursorRequest', { value: true });
      var desc = Object.getOwnPropertyDescriptor;
    }
    var IP_ = IDBIndex.prototype;
    function indexOf(i) { return indexRows(i.objectStore, i.name); }
    def(IP_, 'get', function (query) { var self = this, r = toRange(query); return enqueue(this.objectStore.transaction, this, function () { var rows = indexOf(self); for (var k = 0; k < rows.length; k++) if (r.includes(rows[k].key)) return clone(rows[k].value); return undefined; }); });
    def(IP_, 'getKey', function (query) { var self = this, r = toRange(query); return enqueue(this.objectStore.transaction, this, function () { var rows = indexOf(self); for (var k = 0; k < rows.length; k++) if (r.includes(rows[k].key)) return rows[k].primaryKey; return undefined; }); });
    def(IP_, 'getAll', function (query, count) { var self = this, r = toRange(query); return enqueue(this.objectStore.transaction, this, function () { var out = []; indexOf(self).forEach(function (row) { if ((!r || r.includes(row.key)) && (count === undefined || out.length < count)) out.push(clone(row.value)); }); return out; }); });
    def(IP_, 'getAllKeys', function (query, count) { var self = this, r = toRange(query); return enqueue(this.objectStore.transaction, this, function () { var out = []; indexOf(self).forEach(function (row) { if ((!r || r.includes(row.key)) && (count === undefined || out.length < count)) out.push(row.primaryKey); }); return out; }); });
    def(IP_, 'count', function (query) { var self = this, r = toRange(query); return enqueue(this.objectStore.transaction, this, function () { var n = 0; indexOf(self).forEach(function (row) { if (!r || r.includes(row.key)) n++; }); return n; }); });
    def(IP_, 'openCursor', function (query, direction) { var self = this; return cursorFor(this, function () { return indexOf(self); }, query, direction, true); });
    def(IP_, 'openKeyCursor', function (query, direction) { var self = this; return cursorFor(this, function () { return indexOf(self); }, query, direction, false); });

    function IDBDatabase() { throw new TypeError('Illegal constructor'); }
    IDBDatabase.prototype = Object.create(EventTarget.prototype);
    def(IDBDatabase.prototype, 'constructor', IDBDatabase);
    def(g, 'IDBDatabase', IDBDatabase);
    def(g, 'IDBFactory', function IDBFactory() { throw new TypeError('Illegal constructor'); });
    function connect(data) {
      var db = Object.create(IDBDatabase.prototype);
      db.name = data.name; db.version = data.version; db.onversionchange = null; db.onclose = null; db.onabort = null; db.onerror = null;
      Object.defineProperty(db, '__data', { value: data, writable: true });
      Object.defineProperty(db, '__activeTransactions', { value: 0, writable: true });
      Object.defineProperty(db, '__closed', { value: false, writable: true });
      db.objectStoreNames = list(Object.keys(data.stores));
      return db;
    }
    function refreshNames(db) { db.objectStoreNames = list(Object.keys(db.__data.stores)); }
    var DP_ = IDBDatabase.prototype;
    def(DP_, 'close', function () { this.__closed = true; });
    def(DP_, 'transaction', function (names, mode) {
      if (this.__closed) throw new DOMException("Failed to execute 'transaction' on 'IDBDatabase': The database connection is closing.", 'InvalidStateError');
      names = Array.isArray(names) ? names.map(String) : [String(names)];
      for (var i = 0; i < names.length; i++) if (!this.__data.stores[names[i]]) throw new DOMException("Failed to execute 'transaction' on 'IDBDatabase': One of the specified object stores was not found.", 'NotFoundError');
      if (names.length === 0) throw new DOMException("Failed to execute 'transaction' on 'IDBDatabase': The storeNames parameter was empty.", 'InvalidAccessError');
      var t = newTransaction(this, names, mode || 'readonly');
      this.__activeTransactions++;
      schedule(t);
      return t;
    });
    def(DP_, 'createObjectStore', function (name, options) {
      var t = this.__upgrade;
      if (!t) throw new DOMException("Failed to execute 'createObjectStore' on 'IDBDatabase': The database is not running a version change transaction.", 'InvalidStateError');
      name = String(name);
      if (this.__data.stores[name]) throw new DOMException("Failed to execute 'createObjectStore' on 'IDBDatabase': An object store with the specified name already exists.", 'ConstraintError');
      var keyPath = options && options.keyPath !== undefined && options.keyPath !== null ? options.keyPath : null;
      if (keyPath !== null && options.autoIncrement && (keyPath === '' || Array.isArray(keyPath))) throw new DOMException('autoIncrement requires a key path that is neither empty nor an array.', 'InvalidAccessError');
      this.__data.stores[name] = { keyPath: keyPath, autoIncrement: !!(options && options.autoIncrement), nextKey: 1, records: [], indexes: {} };
      refreshNames(this);
      t.__stores.push(name);
      t.objectStoreNames = list(t.__stores);
      t.__state.wrote = true;
      return newObjectStore(t, name);
    });
    def(DP_, 'deleteObjectStore', function (name) {
      if (!this.__upgrade) throw new DOMException("Failed to execute 'deleteObjectStore' on 'IDBDatabase': The database is not running a version change transaction.", 'InvalidStateError');
      if (!this.__data.stores[name]) throw new DOMException("Failed to execute 'deleteObjectStore' on 'IDBDatabase': The specified object store was not found.", 'NotFoundError');
      delete this.__data.stores[name];
      refreshNames(this);
      this.__upgrade.__state.wrote = true;
    });

    var factory = Object.create(g.IDBFactory.prototype);
    def(g.IDBFactory.prototype, 'open', function (name, version) {
      name = String(name);
      if (version !== undefined && (typeof version !== 'number' || version < 1 || version !== M.floor(version))) throw new TypeError("Failed to execute 'open' on 'IDBFactory': The version provided must be a positive integer.");
      var request = newRequest(null, null, IDBOpenDBRequest.prototype);
      request.onupgradeneeded = null; request.onblocked = null;
      setTimeout(function () {
        var stored = open[name] || load(name), created = false;
        if (!stored) { stored = { name: name, version: 0, stores: {} }; created = true; }
        var target = version === undefined ? (stored.version || 1) : version;
        if (target < stored.version) { request.error = new DOMException('The requested version (' + target + ') is less than the existing version (' + stored.version + ').', 'VersionError'); request.readyState = 'done'; fire(request, 'error', { bubbles: true, cancelable: true }); return; }
        open[name] = stored;
        var db = connect(stored);
        request.result = db;
        if (target > stored.version) {
          var old = stored.version, snapshot = clone(stored);
          stored.version = target; db.version = target;
          var t = newTransaction(db, [], 'versionchange');
          db.__upgrade = t;
          t.__restore = function () { open[name] = snapshot; db.__closed = true; };
          request.transaction = t;
          db.__activeTransactions++;
          var upgrade = new Event('upgradeneeded');
          upgrade.oldVersion = old; upgrade.newVersion = target;
          request.readyState = 'done';
          request.dispatchEvent(upgrade);
          t.__state.wrote = true;
          t.addEventListener('complete', function () { db.__upgrade = null; request.readyState = 'done'; request.result = db; fire(request, 'success'); });
          t.addEventListener('abort', function () { request.error = new DOMException('The upgrade transaction was aborted.', 'AbortError'); request.result = undefined; fire(request, 'error', { bubbles: true, cancelable: true }); });
          schedule(t);
        } else {
          request.readyState = 'done';
          fire(request, 'success');
        }
      }, 0);
      return request;
    });
    def(g.IDBFactory.prototype, 'deleteDatabase', function (name) {
      name = String(name);
      var request = newRequest(null, null, IDBOpenDBRequest.prototype);
      request.onupgradeneeded = null; request.onblocked = null;
      setTimeout(function () {
        var existing = open[name] || load(name);
        delete open[name];
        var ls = store(); if (ls) ls.removeItem(PREFIX + name);
        request.result = undefined; request.readyState = 'done';
        var e = new Event('success'); e.oldVersion = existing ? existing.version : 0; e.newVersion = null;
        request.dispatchEvent(e);
      }, 0);
      return request;
    });
    def(g.IDBFactory.prototype, 'databases', function () {
      var names = {}, ls = store();
      Object.keys(open).forEach(function (n) { names[n] = open[n].version; });
      if (ls) for (var i = 0; i < ls.length; i++) { var k = ls.key(i); if (k && k.indexOf(PREFIX) === 0) { var n2 = k.slice(PREFIX.length); if (names[n2] === undefined) { var d = load(n2); if (d) names[n2] = d.version; } } }
      return Promise.resolve(Object.keys(names).map(function (n) { return { name: n, version: names[n] }; }));
    });
    def(g.IDBFactory.prototype, 'cmp', function (a, b) { if (!valid(a) || !valid(b)) throw dataError('The parameter is not a valid key.'); return compare(a, b); });
    def(g, 'indexedDB', factory);
  }());

  var nativeCreate = DP.createElement;
  method(DP, 'createElement', function (name, options) {
    var el = nativeCreate.call(this, name, options);
    if (definitions[el.localName]) upgrade(el);
    return el;
  });
}(this));
)JSPRELUDE";


const char kWorkerPrelude[] = R"JSPRELUDE((function (g) {
  var hide = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true }); };
  var listeners = { message: [], messageerror: [], error: [] };
  var start = Date.now();
  g.self = g;
  g.name = __worker_name;
  g.onmessage = null; g.onmessageerror = null; g.onerror = null;
  var m = /^([a-z][a-z0-9+.-]*:)(?:\/\/([^\/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(__worker_url) || [];
  g.location = { href: __worker_url, protocol: m[1] || '', host: m[2] || '', hostname: (m[2] || '').replace(/:\d+$/, ''), origin: m[2] ? (m[1] + '//' + m[2]) : 'null', pathname: m[3] || '', search: m[4] || '', hash: m[5] || '', toString: function () { return __worker_url; } };
  g.performance = { now: function () { return Date.now() - start; }, timeOrigin: start };
  g.navigator = { userAgent: navigator.userAgent, language: 'en-US', languages: ['en-US'], hardwareConcurrency: 1, onLine: true };
  g.postMessage = function (message) { if (arguments.length < 1) throw new TypeError("Failed to execute 'postMessage' on 'DedicatedWorkerGlobalScope': 1 argument required, but only 0 present."); __out(__wire_encode(message)); };
  g.close = function () { __worker_close(); };
  g.addEventListener = function (type, fn) { if (listeners[type] && typeof fn === 'function' && listeners[type].indexOf(fn) < 0) listeners[type].push(fn); };
  g.removeEventListener = function (type, fn) { var l = listeners[type], i = l ? l.indexOf(fn) : -1; if (i >= 0) l.splice(i, 1); };
  g.dispatchEvent = function (event) {
    var handler = g['on' + event.type];
    if (typeof handler === 'function') handler.call(g, event);
    var l = listeners[event.type] ? listeners[event.type].slice() : [];
    for (var i = 0; i < l.length; i++) l[i].call(g, event);
    return true;
  };
  g.importScripts = function () {
    for (var i = 0; i < arguments.length; i++) {
      var url = String(arguments[i]);
      var base = __worker_url.slice(0, __worker_url.lastIndexOf('/') + 1);
      if (!/^[a-z][a-z0-9+.-]*:/i.test(url)) url = url.charAt(0) === '/' ? g.location.origin + url : base + url;
      var source = __worker_fetch(url);
      if (source === null) throw new Error("Failed to execute 'importScripts' on 'WorkerGlobalScope': The script at '" + url + "' failed to load.");
      (0, eval)(source);
    }
  };
  // what workers expect to find
  function TextEncoder() { this.encoding = 'utf-8'; }
  TextEncoder.prototype.encode = function (text) { return __string_to_bytes(text === undefined ? '' : String(text)); };
  g.TextEncoder = TextEncoder;
  function TextDecoder(label) { this.encoding = 'utf-8'; this.fatal = false; this.ignoreBOM = false; }
  TextDecoder.prototype.decode = function (input) {
    if (input === undefined) return '';
    var bytes = input instanceof ArrayBuffer ? new Uint8Array(input) : (ArrayBuffer.isView(input) ? new Uint8Array(input.buffer, input.byteOffset, input.byteLength) : null);
    if (!bytes) throw new TypeError("Failed to execute 'decode' on 'TextDecoder': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
    if (bytes.length >= 3 && bytes[0] === 0xEF && bytes[1] === 0xBB && bytes[2] === 0xBF) bytes = bytes.subarray(3);
    return __bytes_to_string(bytes);
  };
  g.TextDecoder = TextDecoder;
  var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  g.btoa = function (text) {
    text = String(text);
    var out = '';
    for (var i = 0; i < text.length; i += 3) {
      var a = text.charCodeAt(i), b = text.charCodeAt(i + 1), c = text.charCodeAt(i + 2);
      if (a > 255 || b > 255 || c > 255) throw new Error("Failed to execute 'btoa' on 'WorkerGlobalScope': The string to be encoded contains characters outside of the Latin1 range.");
      out += B64.charAt(a >> 2) + B64.charAt(((a & 3) << 4) | ((b || 0) >> 4)) + (i + 1 < text.length ? B64.charAt(((b & 15) << 2) | ((c || 0) >> 6)) : '=') + (i + 2 < text.length ? B64.charAt(c & 63) : '=');
    }
    return out;
  };
  g.atob = function (text) {
    text = String(text).replace(/[\t\n\f\r ]/g, '');
    if (text.length % 4 === 1 || /[^A-Za-z0-9+\/=]/.test(text)) throw new Error("Failed to execute 'atob' on 'WorkerGlobalScope': The string to be decoded is not correctly encoded.");
    var bytes = [];
    for (var i = 0; i < text.length; i += 4) {
      var n = [0, 1, 2, 3].map(function (k) { var ch = text.charAt(i + k); return ch === '' || ch === '=' ? -1 : B64.indexOf(ch); });
      bytes.push((n[0] << 2) | (n[1] >> 4));
      if (n[2] >= 0) bytes.push(((n[1] & 15) << 4) | (n[2] >> 2));
      if (n[3] >= 0) bytes.push(((n[2] & 3) << 6) | n[3]);
    }
    return bytes.map(function (b) { return String.fromCharCode(b); }).join('');
  };
  // fetch() for a worker: the answer is had at once (only GET, only text), and comes as a promise
  g.fetch = function (resource) {
    var url = typeof resource === 'string' ? resource : resource && resource.url ? resource.url : String(resource);
    if (!/^[a-z][a-z0-9+.-]*:/i.test(url)) url = url.charAt(0) === '/' ? g.location.origin + url : __worker_url.slice(0, __worker_url.lastIndexOf('/') + 1) + url;
    var text = __worker_fetch(url);
    if (text === null) return Promise.reject(new TypeError('Failed to fetch'));
    var response = {
      ok: true, status: 200, statusText: 'OK', url: url, type: 'basic', redirected: false, headers: { get: function () { return null; }, has: function () { return false; } }, bodyUsed: false,
      text: function () { return Promise.resolve(text); },
      json: function () { try { return Promise.resolve(JSON.parse(text)); } catch (e) { return Promise.reject(e); } },
      arrayBuffer: function () { return Promise.resolve(__string_to_bytes(text).buffer); },
      clone: function () { return response; }
    };
    return Promise.resolve(response);
  };
  hide(g, '__deliver', function () {
    var text = g.__inbox, data;
    try { data = __wire_decode(text); } catch (e) { g.dispatchEvent({ type: 'messageerror', data: null, target: g }); return; }
    try { g.dispatchEvent({ type: 'message', data: data, origin: '', lastEventId: '', source: null, ports: [], target: g }); }
    catch (e) { __worker_fail(String(e && e.message !== undefined ? (e.name || 'Error') + ': ' + e.message : e)); }
  });
}(this));
)JSPRELUDE";

}  // namespace

const char* web_prelude() { return kPrelude; }
const char* worker_prelude() { return kWorkerPrelude; }
const char* dom_prelude() { return kDomPrelude; }

}  // namespace browser
