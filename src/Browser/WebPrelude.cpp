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
      if (typeof Map === 'function' && v instanceof Map) { var pairs = []; v.forEach(function (val, key) { pairs.push([enc(key), enc(val)]); }); return { $: 'm', e: pairs }; }
      if (typeof Set === 'function' && v instanceof Set) { var members = []; v.forEach(function (val) { members.push(enc(val)); }); return { $: 's', e: members }; }
      if (v instanceof RegExp) return { $: 're', s: v.source, f: v.flags };
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
        case 'm': out = new Map(); objs.push(out); for (i = 0; i < e.e.length; i++) out.set(dec(e.e[i][0]), dec(e.e[i][1])); return out;
        case 's': out = new Set(); objs.push(out); for (i = 0; i < e.e.length; i++) out.add(dec(e.e[i])); return out;
        case 're': out = new RegExp(e.s, e.f); objs.push(out); return out;
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
  // --- Picture-in-Picture API. The browser shows the video in a window of its own; the promises, the window object and the events are here.
  function PictureInPictureWindow() { throw new TypeError('Illegal constructor'); }
  PictureInPictureWindow.prototype = Object.create(EventTarget.prototype);
  method(PictureInPictureWindow.prototype, 'constructor', PictureInPictureWindow);
  function PictureInPictureEvent(type, init) {
    if (arguments.length < 2 || !init || !(init.pictureInPictureWindow instanceof PictureInPictureWindow)) throw new TypeError("Failed to construct 'PictureInPictureEvent': required member pictureInPictureWindow is undefined.");
    var event = new Event(type, init);
    Object.setPrototypeOf(event, PictureInPictureEvent.prototype);
    Object.defineProperty(event, 'pictureInPictureWindow', { value: init.pictureInPictureWindow, enumerable: true });
    return event;
  }
  PictureInPictureEvent.prototype = Object.create(Event.prototype);
  method(PictureInPictureEvent.prototype, 'constructor', PictureInPictureEvent);
  var pipState = null;   // { video, window }
  var pipNotice = function (video, type, win) { video.dispatchEvent(new PictureInPictureEvent(type, { bubbles: true, pictureInPictureWindow: win })); };
  var pipDom = function (message, name) { return new DOMException("Failed to execute 'requestPictureInPicture' on 'HTMLVideoElement': " + message, name); };
  if (g.HTMLVideoElement) {
    var VP = g.HTMLVideoElement.prototype;
    method(VP, 'requestPictureInPicture', function () {
      var video = this;
      if (!__pip_supported()) return Promise.reject(pipDom('Picture-in-Picture is not supported.', 'NotSupportedError'));
      var reply = __pip_request(video);
      if (reply[0] === 1) return Promise.reject(pipDom('Picture-in-Picture is not supported.', 'NotSupportedError'));
      if (reply[0] === 2) return Promise.reject(pipDom('The video element has no video to show yet (readyState is HAVE_NOTHING).', 'InvalidStateError'));
      if (reply[0] === 3) return Promise.reject(pipDom('Picture-in-Picture is disabled for this video (disablePictureInPicture).', 'InvalidStateError'));
      var previous = pipState, win = pipState && pipState.video === video ? pipState.window : null;
      if (!win) {
        win = Object.create(PictureInPictureWindow.prototype);
        win.onresize = null;
      }
      Object.defineProperty(win, 'width', { value: reply[1], configurable: true, enumerable: true });
      Object.defineProperty(win, 'height', { value: reply[2], configurable: true, enumerable: true });
      pipState = { video: video, window: win };
      return new Promise(function (resolve) {
        setTimeout(function () {
          if (previous && previous.video !== video) pipNotice(previous.video, 'leavepictureinpicture', previous.window);
          if (!previous || previous.video !== video) pipNotice(video, 'enterpictureinpicture', win);
          resolve(win);
        }, 0);
      });
    });
    var pipDisabled = Object.getOwnPropertyDescriptor(VP, 'disablePictureInPicture');
    if (!pipDisabled) Object.defineProperty(VP, 'disablePictureInPicture', { get: function () { return this.hasAttribute('disablepictureinpicture'); },
      set: function (v) { if (v) this.setAttribute('disablepictureinpicture', ''); else this.removeAttribute('disablepictureinpicture'); }, configurable: true, enumerable: true });
  }
  method(DP, 'exitPictureInPicture', function () {
    var state = pipState;
    if (!state) return Promise.reject(new DOMException("Failed to execute 'exitPictureInPicture' on 'Document': There is no Picture-in-Picture element in this document.", 'InvalidStateError'));
    pipState = null;
    __pip_exit();
    return new Promise(function (resolve) { setTimeout(function () { pipNotice(state.video, 'leavepictureinpicture', state.window); resolve(); }, 0); });
  });
  Object.defineProperty(DP, 'pictureInPictureElement', { get: function () { return pipState ? __pip_element() : null; }, configurable: true, enumerable: true });
  Object.defineProperty(DP, 'pictureInPictureEnabled', { get: function () { return !!__pip_supported(); }, configurable: true, enumerable: true });
  // The user closed the window, or changed its size.
  hide(g, '__pip_left', function () {
    var state = pipState;
    pipState = null;
    if (state) setTimeout(function () { pipNotice(state.video, 'leavepictureinpicture', state.window); }, 0);
  });
  hide(g, '__pip_resized', function (w, h) {
    if (!pipState) return;
    var win = pipState.window;
    Object.defineProperty(win, 'width', { value: w, configurable: true, enumerable: true });
    Object.defineProperty(win, 'height', { value: h, configurable: true, enumerable: true });
    setTimeout(function () { win.dispatchEvent(new Event('resize')); }, 0);
  });
  hide(g, 'PictureInPictureWindow', PictureInPictureWindow);
  hide(g, 'PictureInPictureEvent', PictureInPictureEvent);
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
  // @@PLATFORM1

  // ====================================================================================================================================================
  // The rest of window, document, navigator and location (the parts of the platform that need no native help): what a page may read to find out what this
  // browser can do - and, where the machine cannot do it (cameras, location, notifications), the answer the standard gives for that: a denied permission, an
  // empty list, a rejected promise.
  // ====================================================================================================================================================
  var typeErr = function (what, message) { return new TypeError("Failed to " + what + ": " + message); };
  var defineGetter = function (o, name, get, set) { Object.defineProperty(o, name, { get: get, set: set, configurable: true, enumerable: true }); };
  var tag = function (ctor, name) { try { Object.defineProperty(ctor.prototype, Symbol.toStringTag, { value: name, configurable: true }); } catch (e) { } };
  var klass = function (name, parent, init) {   // an interface of script: new Name() only where `init` allows
    var f = function () {
      if (!init) throw new TypeError('Illegal constructor');
      if (!(this instanceof f)) throw typeErr("construct '" + name + "'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      init.apply(this, arguments);
    };
    Object.defineProperty(f, 'name', { value: name });
    f.prototype = Object.create(parent ? parent.prototype : Object.prototype);
    Object.defineProperty(f.prototype, 'constructor', { value: f, writable: true, configurable: true });
    tag(f, name);
    if (!(name in g)) hide(g, name, f);   // (an interface the browser already has stays: the native code holds its prototype)
    return f;
  };
  var ET = g.EventTarget;
  var later = function (fn) { setTimeout(fn, 0); };
  var denied = function (what) { return new DOMException(what, 'NotAllowedError'); };

  // ---------------------------------------------------------------------------------------------------------------------------------- window
  var BarProp = klass('BarProp', null);
  ['locationbar', 'menubar', 'personalbar', 'scrollbars', 'statusbar', 'toolbar'].forEach(function (name) {
    var bar = Object.create(BarProp.prototype);
    Object.defineProperty(bar, 'visible', { get: function () { return true; }, enumerable: true });
    if (!(name in g)) defineGetter(g, name, function () { return bar; });
  });
  if (!('clientInformation' in g)) defineGetter(g, 'clientInformation', function () { return g.navigator; });
  if (!('originAgentCluster' in g)) hide(g, 'originAgentCluster', false);
  if (!('crossOriginIsolated' in g)) hide(g, 'crossOriginIsolated', false);
  if (!('captureEvents' in g)) hide(g, 'captureEvents', function () { });
  if (!('releaseEvents' in g)) hide(g, 'releaseEvents', function () { });
  if (!('moveBy' in g)) hide(g, 'moveBy', function () { });
  if (!('resizeBy' in g)) hide(g, 'resizeBy', function () { });
  if (!('find' in g)) hide(g, 'find', function () { return false; });
  if (!('event' in g)) defineGetter(g, 'event', function () { return g.__current_event; });
  if (!('external' in g)) hide(g, 'external', { AddSearchProvider: function () { }, IsSearchProviderInstalled: function () { return 0; } });
  if (!('reportError' in g)) hide(g, 'reportError', function (error) {
    if (arguments.length < 1) throw typeErr("execute 'reportError' on 'Window'", '1 argument required, but only 0 present.');
    var message = error && error.message !== undefined ? String(error.message) : String(error);
    var e = new ErrorEvent('error', { message: 'Uncaught ' + (error && error.name ? error.name + ': ' : '') + message, error: error, cancelable: true });
    if (g.dispatchEvent(e)) console.error(e.message);
  });
  // window.open: a browser window of this kind has one page; "_self" and its kin navigate it, anything else is a popup that is not allowed (null).
  var nativeOpen = g.open;
  hide(g, 'open', function (url, target) {
    target = target === undefined ? '_blank' : String(target);
    url = url === undefined ? '' : String(url);
    if ((target === '_self' || target === '_top' || target === '_parent') && url !== '' && url !== 'about:blank') { g.location.assign(url); return g; }
    return null;
  });
  // The visual viewport is the layout viewport here (no pinch zoom).
  var VisualViewport = klass('VisualViewport', ET);
  var vv = Object.create(VisualViewport.prototype);
  [['offsetLeft', 0], ['offsetTop', 0], ['pageLeft', function () { return g.scrollX; }], ['pageTop', function () { return g.scrollY; }], ['width', function () { return g.innerWidth; }],
   ['height', function () { return g.innerHeight; }], ['scale', 1]].forEach(function (p) {
    defineGetter(VisualViewport.prototype, p[0], function () { return typeof p[1] === 'function' ? p[1]() : p[1]; });
  });
  vv.onresize = null; vv.onscroll = null; vv.onscrollend = null;
  if (!('visualViewport' in g)) defineGetter(g, 'visualViewport', function () { return vv; });
  // speech
  var SpeechSynthesis = klass('SpeechSynthesis', ET);
  var speech = Object.create(SpeechSynthesis.prototype);
  ['speaking', 'pending', 'paused'].forEach(function (n) { defineGetter(SpeechSynthesis.prototype, n, function () { return false; }); });
  ['speak', 'cancel', 'pause', 'resume'].forEach(function (n) { method(SpeechSynthesis.prototype, n, function () { }); });
  method(SpeechSynthesis.prototype, 'getVoices', function () { return []; });
  speech.onvoiceschanged = null;
  var SpeechSynthesisUtterance = klass('SpeechSynthesisUtterance', ET, function (text) {
    this.text = text === undefined ? '' : String(text); this.lang = ''; this.voice = null; this.volume = 1; this.rate = 1; this.pitch = 1;
    this.onstart = this.onend = this.onerror = this.onpause = this.onresume = this.onmark = this.onboundary = null;
  });
  if (!('speechSynthesis' in g)) defineGetter(g, 'speechSynthesis', function () { return speech; });
  // trusted types (policies that pass strings through: this browser has no sinks that enforce them)
  var TrustedHTML = klass('TrustedHTML', null), TrustedScript = klass('TrustedScript', null), TrustedScriptURL = klass('TrustedScriptURL', null);
  var trusted = function (C, v) { var o = Object.create(C.prototype); hide(o, '__v', String(v)); return o; };
  [TrustedHTML, TrustedScript, TrustedScriptURL].forEach(function (C) { method(C.prototype, 'toString', function () { return this.__v; }); method(C.prototype, 'toJSON', function () { return this.__v; }); });
  var TrustedTypePolicy = klass('TrustedTypePolicy', null);
  var TrustedTypePolicyFactory = klass('TrustedTypePolicyFactory', null);
  var ttf = Object.create(TrustedTypePolicyFactory.prototype), policies = {};
  method(TrustedTypePolicyFactory.prototype, 'createPolicy', function (name, rules) {
    name = String(name);
    if (policies[name] && name !== 'default') throw typeErr("execute 'createPolicy' on 'TrustedTypePolicyFactory'", "Policy with name \"" + name + "\" already exists.");
    var p = Object.create(TrustedTypePolicy.prototype);
    Object.defineProperty(p, 'name', { value: name, enumerable: true });
    var make = function (C, key) {
      return function (input) {
        var args = Array.prototype.slice.call(arguments, 1);
        if (!rules || typeof rules[key] !== 'function') throw typeErr("execute 'create" + key.slice(6) + "' on 'TrustedTypePolicy'", 'Policy ' + name + "'s TrustedType" + key.slice(6) + ' constructor is undefined.');
        return trusted(C, rules[key].apply(rules, [String(input)].concat(args)));
      };
    };
    method(p, 'createHTML', make(TrustedHTML, 'createHTML'));
    method(p, 'createScript', make(TrustedScript, 'createScript'));
    method(p, 'createScriptURL', make(TrustedScriptURL, 'createScriptURL'));
    policies[name] = p;
    return p;
  });
  method(TrustedTypePolicyFactory.prototype, 'isHTML', function (v) { return v instanceof TrustedHTML; });
  method(TrustedTypePolicyFactory.prototype, 'isScript', function (v) { return v instanceof TrustedScript; });
  method(TrustedTypePolicyFactory.prototype, 'isScriptURL', function (v) { return v instanceof TrustedScriptURL; });
  method(TrustedTypePolicyFactory.prototype, 'getAttributeType', function () { return null; });
  method(TrustedTypePolicyFactory.prototype, 'getPropertyType', function () { return null; });
  defineGetter(TrustedTypePolicyFactory.prototype, 'emptyHTML', function () { return trusted(TrustedHTML, ''); });
  defineGetter(TrustedTypePolicyFactory.prototype, 'emptyScript', function () { return trusted(TrustedScript, ''); });
  defineGetter(TrustedTypePolicyFactory.prototype, 'defaultPolicy', function () { return policies['default'] || null; });
  if (!('trustedTypes' in g)) defineGetter(g, 'trustedTypes', function () { return ttf; });

  // ---------------------------------------------------------------------------------------------------------------------------------- screen, history, location
  (function () {
    var s = g.screen;
    if (s) {
      if (!('availLeft' in s)) Object.defineProperty(s, 'availLeft', { value: 0, enumerable: true });
      if (!('availTop' in s)) Object.defineProperty(s, 'availTop', { value: 0, enumerable: true });
      if (!('isExtended' in s)) Object.defineProperty(s, 'isExtended', { value: false, enumerable: true });
    }
    var Screen = klass('Screen', ET), ScreenOrientation = klass('ScreenOrientation', ET);
    if (s) { try { Object.setPrototypeOf(s, Screen.prototype); if (s.orientation) { Object.setPrototypeOf(s.orientation, ScreenOrientation.prototype); s.orientation.onchange = null; s.orientation.lock = function () { return Promise.reject(new DOMException('screen.orientation.lock() is not available on this device.', 'NotSupportedError')); }; s.orientation.unlock = function () { }; } } catch (e) { } }
    var DOMStringList = klass('DOMStringList', null);
    accessorOf(DOMStringList.prototype, 'length', function () { return 0; });
    method(DOMStringList.prototype, 'item', function () { return null; });
    method(DOMStringList.prototype, 'contains', function () { return false; });
    if (g.location && !('ancestorOrigins' in g.location)) {
      var empty = Object.create(DOMStringList.prototype);
      try { Object.defineProperty(g.location, 'ancestorOrigins', { value: empty, enumerable: true }); } catch (e) { }
    }
  }());
  function accessorOf(o, name, get, set) { Object.defineProperty(o, name, { get: get, set: set, enumerable: true, configurable: true }); }

  // ---------------------------------------------------------------------------------------------------------------------------------- navigator
  (function () {
    var N = g.navigator, NP2 = Object.getPrototypeOf(N);
    var Navigator = klass('Navigator', null);
    try { Object.setPrototypeOf(N, Navigator.prototype); } catch (e) { }
    var put = function (name, value) { if (!(name in N)) Object.defineProperty(N, name, typeof value === 'function' && value.__getter ? { get: value, enumerable: true, configurable: true } : { value: value, enumerable: true, writable: true, configurable: true }); };
    var getter = function (fn) { fn.__getter = true; return fn; };
    put('appCodeName', 'Mozilla'); put('appName', 'Netscape'); put('product', 'Gecko'); put('productSub', '20030107'); put('vendorSub', '');
    put('appVersion', '5.0 (TheoryCraftOS) ' + String(N.userAgent).replace(/^Mozilla\/5\.0 /, ''));
    put('deviceMemory', 4); put('maxTouchPoints', 0); put('pdfViewerEnabled', false); put('webdriver', false);
    put('javaEnabled', function () { return false; });
    put('vibrate', function () { return false; });
    put('getGamepads', function () { return []; });
    put('registerProtocolHandler', function () { });
    put('unregisterProtocolHandler', function () { });
    put('setAppBadge', function () { return Promise.resolve(); });
    put('clearAppBadge', function () { return Promise.resolve(); });
    put('canShare', function () { return false; });
    put('share', function () { return Promise.reject(denied("Failed to execute 'share' on 'Navigator': Must be handling a user gesture to perform a share request.")); });
    put('sendBeacon', function (url, data) {
      if (arguments.length < 1) throw typeErr("execute 'sendBeacon' on 'Navigator'", '1 argument required, but only 0 present.');
      try { g.fetch(String(url), { method: 'POST', body: data === undefined ? null : data, keepalive: true }).then(null, function () { }); return true; } catch (e) { return false; }
    });
    // plugins and mime types: none (the built-in PDF viewer is not there)
    var PluginArray = klass('PluginArray', null), MimeTypeArray = klass('MimeTypeArray', null), Plugin = klass('Plugin', null), MimeType = klass('MimeType', null);
    [PluginArray, MimeTypeArray].forEach(function (C) {
      accessorOf(C.prototype, 'length', function () { return 0; });
      method(C.prototype, 'item', function () { return null; }); method(C.prototype, 'namedItem', function () { return null; });
    });
    method(PluginArray.prototype, 'refresh', function () { });
    var plugins = Object.create(PluginArray.prototype), mimeTypes = Object.create(MimeTypeArray.prototype);
    put('plugins', getter(function () { return plugins; })); put('mimeTypes', getter(function () { return mimeTypes; }));
    // permissions: nothing that needs asking is granted
    var PermissionStatus = klass('PermissionStatus', ET), Permissions = klass('Permissions', null);
    var known = ['geolocation', 'notifications', 'push', 'midi', 'camera', 'microphone', 'speaker-selection', 'device-info', 'background-fetch', 'background-sync', 'bluetooth', 'persistent-storage',
                 'ambient-light-sensor', 'accelerometer', 'gyroscope', 'magnetometer', 'clipboard-read', 'clipboard-write', 'display-capture', 'screen-wake-lock', 'nfc', 'storage-access', 'window-management', 'local-fonts'];
    var granted = { 'clipboard-write': true, 'screen-wake-lock': false };
    var statuses = {};
    var statusFor = function (name) {
      if (statuses[name]) return statuses[name];
      var st = Object.create(PermissionStatus.prototype);
      Object.defineProperty(st, 'name', { value: name, enumerable: true });
      Object.defineProperty(st, 'state', { get: function () { return granted[name] ? 'granted' : (name === 'notifications' || name === 'push' ? 'denied' : 'prompt'); }, enumerable: true });
      st.onchange = null;
      return (statuses[name] = st);
    };
    method(Permissions.prototype, 'query', function (descriptor) {
      if (arguments.length < 1) return Promise.reject(typeErr("execute 'query' on 'Permissions'", '1 argument required, but only 0 present.'));
      if (descriptor === null || typeof descriptor !== 'object' || descriptor.name === undefined) return Promise.reject(typeErr("execute 'query' on 'Permissions'", "required member name is undefined."));
      var name = String(descriptor.name);
      if (known.indexOf(name) < 0) return Promise.reject(typeErr("execute 'query' on 'Permissions'", "The provided value '" + name + "' is not a valid enum value of type PermissionName."));
      return Promise.resolve(statusFor(name));
    });
    method(Permissions.prototype, 'request', function (d) { return this.query(d); });
    method(Permissions.prototype, 'revoke', function (d) { return this.query(d); });
    var permissions = Object.create(Permissions.prototype);
    put('permissions', getter(function () { return permissions; }));
    // geolocation: no position can be had
    var GeolocationPositionError = klass('GeolocationPositionError', null), Geolocation = klass('Geolocation', null);
    GeolocationPositionError.PERMISSION_DENIED = 1; GeolocationPositionError.POSITION_UNAVAILABLE = 2; GeolocationPositionError.TIMEOUT = 3;
    GeolocationPositionError.prototype.PERMISSION_DENIED = 1; GeolocationPositionError.prototype.POSITION_UNAVAILABLE = 2; GeolocationPositionError.prototype.TIMEOUT = 3;
    var watchId = 0;
    var geoFail = function (error) {
      if (typeof error !== 'function') return;
      later(function () { var e = Object.create(GeolocationPositionError.prototype); e.code = 1; e.message = 'User denied Geolocation'; error(e); });
    };
    method(Geolocation.prototype, 'getCurrentPosition', function (success, error) {
      if (arguments.length < 1 || typeof success !== 'function') throw typeErr("execute 'getCurrentPosition' on 'Geolocation'", "The callback provided as parameter 1 is not a function.");
      geoFail(error);
    });
    method(Geolocation.prototype, 'watchPosition', function (success, error) {
      if (arguments.length < 1 || typeof success !== 'function') throw typeErr("execute 'watchPosition' on 'Geolocation'", "The callback provided as parameter 1 is not a function.");
      geoFail(error);
      return ++watchId;
    });
    method(Geolocation.prototype, 'clearWatch', function () { });
    var geolocation = Object.create(Geolocation.prototype);
    put('geolocation', getter(function () { return geolocation; }));
    // media devices: none
    var MediaDevices = klass('MediaDevices', ET), MediaStream = g.MediaStream || klass('MediaStream', ET, function () { this.id = 'stream'; this.active = false; });
    method(MediaStream.prototype, 'getTracks', function () { return []; }); method(MediaStream.prototype, 'getAudioTracks', function () { return []; }); method(MediaStream.prototype, 'getVideoTracks', function () { return []; });
    var MediaDeviceInfo = klass('MediaDeviceInfo', null);
    var devices = Object.create(MediaDevices.prototype);
    method(MediaDevices.prototype, 'enumerateDevices', function () { return Promise.resolve([]); });
    method(MediaDevices.prototype, 'getSupportedConstraints', function () { return {}; });
    method(MediaDevices.prototype, 'getUserMedia', function () { return Promise.reject(new DOMException('Requested device not found', 'NotFoundError')); });
    method(MediaDevices.prototype, 'getDisplayMedia', function () { return Promise.reject(denied('Permission denied')); });
    devices.ondevicechange = null;
    put('mediaDevices', getter(function () { return devices; }));
    // clipboard (the text of the browser's clipboard)
    var Clipboard = klass('Clipboard', ET), ClipboardItem = klass('ClipboardItem', null, function (items) {
      if (items === null || typeof items !== 'object') throw typeErr("construct 'ClipboardItem'", 'The provided value is not of type record.');
      hide(this, '__items', items);
    });
    defineGetter(ClipboardItem.prototype, 'types', function () { return Object.keys(this.__items); });
    method(ClipboardItem.prototype, 'getType', function (type) {
      var v = this.__items[type];
      if (v === undefined) return Promise.reject(new DOMException("Failed to execute 'getType' on 'ClipboardItem': The type '" + type + "' was not found", 'NotFoundError'));
      return Promise.resolve(v).then(function (x) { return x instanceof Blob ? x : new Blob([String(x)], { type: type }); });
    });
    ClipboardItem.supports = function (type) { return type === 'text/plain' || type === 'text/html'; };
    method(Clipboard.prototype, 'writeText', function (text) {
      if (arguments.length < 1) return Promise.reject(typeErr("execute 'writeText' on 'Clipboard'", '1 argument required, but only 0 present.'));
      __clipboard_set(String(text));
      return Promise.resolve();
    });
    method(Clipboard.prototype, 'readText', function () { var t = __clipboard_get(); return Promise.resolve(t === null ? '' : t); });
    method(Clipboard.prototype, 'write', function (items) {
      if (!items || !items.length) return Promise.resolve();
      var item = items[0];
      return item.getType('text/plain').then(function (b) { return b.text(); }, function () { return ''; }).then(function (t) { __clipboard_set(t); });
    });
    method(Clipboard.prototype, 'read', function () {
      var t = __clipboard_get();
      return Promise.resolve(t === null || t === '' ? [] : [new ClipboardItem({ 'text/plain': new Blob([t], { type: 'text/plain' }) })]);
    });
    var clipboard = Object.create(Clipboard.prototype);
    put('clipboard', getter(function () { return clipboard; }));
    // storage manager, credentials, locks, wake lock, battery, connection, user agent data, media session, service workers, keyboard
    var StorageManager = klass('StorageManager', null), storage = Object.create(StorageManager.prototype);
    method(StorageManager.prototype, 'estimate', function () { return Promise.resolve({ usage: 0, quota: 134217728 }); });
    method(StorageManager.prototype, 'persist', function () { return Promise.resolve(false); });
    method(StorageManager.prototype, 'persisted', function () { return Promise.resolve(false); });
    method(StorageManager.prototype, 'getDirectory', function () { return Promise.reject(new DOMException('Storage directory access is denied.', 'SecurityError')); });
    put('storage', getter(function () { return storage; }));
    var CredentialsContainer = klass('CredentialsContainer', null), credentials = Object.create(CredentialsContainer.prototype);
    method(CredentialsContainer.prototype, 'get', function () { return Promise.resolve(null); });
    method(CredentialsContainer.prototype, 'store', function (c) { return Promise.resolve(c); });
    method(CredentialsContainer.prototype, 'create', function () { return Promise.resolve(null); });
    method(CredentialsContainer.prototype, 'preventSilentAccess', function () { return Promise.resolve(); });
    put('credentials', getter(function () { return credentials; }));
    var LockManager = klass('LockManager', null), Lock = klass('Lock', null), locks = Object.create(LockManager.prototype), held = {}, waiting = {};
    method(LockManager.prototype, 'request', function (name, options, callback) {
      if (typeof options === 'function') { callback = options; options = {}; }
      options = options || {};
      name = String(name);
      if (typeof callback !== 'function') return Promise.reject(typeErr("execute 'request' on 'LockManager'", "parameter 2 is not of type 'Function'."));
      if (name.charAt(0) === '-') return Promise.reject(new DOMException("Failed to execute 'request' on 'LockManager': Names cannot start with '-'.", 'NotSupportedError'));
      var mode = options.mode === 'shared' ? 'shared' : 'exclusive';
      return new Promise(function (resolve, reject) {
        var attempt = function () {
          var h = held[name];
          if (h && (h.mode === 'exclusive' || mode === 'exclusive')) {
            if (options.ifAvailable) { Promise.resolve().then(function () { return callback(null); }).then(resolve, reject); return; }
            (waiting[name] = waiting[name] || []).push(attempt);
            return;
          }
          var lock = Object.create(Lock.prototype);
          Object.defineProperty(lock, 'name', { value: name, enumerable: true }); Object.defineProperty(lock, 'mode', { value: mode, enumerable: true });
          held[name] = { mode: mode, count: ((h && h.count) || 0) + 1 };
          var done = function () {
            held[name].count--;
            if (held[name].count === 0) delete held[name];
            var next = (waiting[name] || []).shift();
            if (next) next();
          };
          Promise.resolve().then(function () { return callback(lock); }).then(function (v) { done(); resolve(v); }, function (e) { done(); reject(e); });
        };
        attempt();
      });
    });
    method(LockManager.prototype, 'query', function () {
      var list = Object.keys(held).map(function (n) { return { name: n, mode: held[n].mode, clientId: 'page' }; });
      return Promise.resolve({ held: list, pending: [] });
    });
    put('locks', getter(function () { return locks; }));
    var WakeLock = klass('WakeLock', null), wakeLock = Object.create(WakeLock.prototype);
    method(WakeLock.prototype, 'request', function () { return Promise.reject(denied('Wake Lock permission request denied')); });
    put('wakeLock', getter(function () { return wakeLock; }));
    var BatteryManager = klass('BatteryManager', ET), battery = Object.create(BatteryManager.prototype);
    [['charging', true], ['chargingTime', 0], ['dischargingTime', Infinity], ['level', 1]].forEach(function (p) { defineGetter(BatteryManager.prototype, p[0], function () { return p[1]; }); });
    battery.onchargingchange = battery.onchargingtimechange = battery.ondischargingtimechange = battery.onlevelchange = null;
    put('getBattery', function () { return Promise.resolve(battery); });
    var NetworkInformation = klass('NetworkInformation', ET), connection = Object.create(NetworkInformation.prototype);
    [['effectiveType', '4g'], ['downlink', 10], ['rtt', 50], ['saveData', false], ['type', 'unknown'], ['downlinkMax', Infinity]].forEach(function (p) { defineGetter(NetworkInformation.prototype, p[0], function () { return p[1]; }); });
    connection.onchange = null;
    put('connection', getter(function () { return connection; }));
    var NavigatorUAData = klass('NavigatorUAData', null), uad = Object.create(NavigatorUAData.prototype);
    defineGetter(NavigatorUAData.prototype, 'brands', function () { return [{ brand: 'TheoryCraftOS', version: '1' }]; });
    defineGetter(NavigatorUAData.prototype, 'mobile', function () { return false; });
    defineGetter(NavigatorUAData.prototype, 'platform', function () { return 'TheoryCraftOS'; });
    method(NavigatorUAData.prototype, 'getHighEntropyValues', function (hints) {
      var out = { brands: this.brands, mobile: false, platform: 'TheoryCraftOS' };
      (hints || []).forEach(function (h) { out[h] = h === 'architecture' ? 'x86' : h === 'bitness' ? '64' : h === 'model' ? '' : h === 'platformVersion' ? '1.0' : h === 'uaFullVersion' ? '1.0' : h === 'wow64' ? false : ''; });
      return Promise.resolve(out);
    });
    method(NavigatorUAData.prototype, 'toJSON', function () { return { brands: this.brands, mobile: false, platform: 'TheoryCraftOS' }; });
    put('userAgentData', getter(function () { return uad; }));
    var MediaSession = klass('MediaSession', null), MediaMetadata = klass('MediaMetadata', null, function (init) {
      init = init || {};
      this.title = init.title === undefined ? '' : String(init.title); this.artist = init.artist === undefined ? '' : String(init.artist);
      this.album = init.album === undefined ? '' : String(init.album); this.artwork = init.artwork || [];
    });
    var session = Object.create(MediaSession.prototype), handlers = {};
    session.metadata = null; session.playbackState = 'none';
    method(MediaSession.prototype, 'setActionHandler', function (action, fn) { handlers[String(action)] = fn; });
    method(MediaSession.prototype, 'setPositionState', function () { });
    method(MediaSession.prototype, 'setCameraActive', function () { });
    method(MediaSession.prototype, 'setMicrophoneActive', function () { });
    put('mediaSession', getter(function () { return session; }));
    var ServiceWorkerContainer = klass('ServiceWorkerContainer', ET), sw = Object.create(ServiceWorkerContainer.prototype);
    sw.controller = null; sw.oncontrollerchange = null; sw.onmessage = null; sw.onmessageerror = null;
    defineGetter(ServiceWorkerContainer.prototype, 'ready', function () { return new Promise(function () { }); });
    method(ServiceWorkerContainer.prototype, 'register', function () { return Promise.reject(new DOMException('Failed to register a ServiceWorker: service workers are not supported by this browser.', 'SecurityError')); });
    method(ServiceWorkerContainer.prototype, 'getRegistration', function () { return Promise.resolve(undefined); });
    method(ServiceWorkerContainer.prototype, 'getRegistrations', function () { return Promise.resolve([]); });
    method(ServiceWorkerContainer.prototype, 'startMessages', function () { });
    put('serviceWorker', getter(function () { return sw; }));
    var Keyboard = klass('Keyboard', ET), keyboard = Object.create(Keyboard.prototype);
    method(Keyboard.prototype, 'getLayoutMap', function () { return Promise.resolve(new Map()); });
    method(Keyboard.prototype, 'lock', function () { return Promise.resolve(); });
    method(Keyboard.prototype, 'unlock', function () { });
    put('keyboard', getter(function () { return keyboard; }));
    var VirtualKeyboard = klass('VirtualKeyboard', ET), vk = Object.create(VirtualKeyboard.prototype);
    vk.overlaysContent = false; vk.ongeometrychange = null;
    defineGetter(VirtualKeyboard.prototype, 'boundingRect', function () { return new DOMRect(0, 0, 0, 0); });
    method(VirtualKeyboard.prototype, 'show', function () { }); method(VirtualKeyboard.prototype, 'hide', function () { });
    put('virtualKeyboard', getter(function () { return vk; }));
    var WindowControlsOverlay = klass('WindowControlsOverlay', ET), wco = Object.create(WindowControlsOverlay.prototype);
    defineGetter(WindowControlsOverlay.prototype, 'visible', function () { return false; });
    method(WindowControlsOverlay.prototype, 'getTitlebarAreaRect', function () { return new DOMRect(0, 0, 0, 0); });
    put('windowControlsOverlay', getter(function () { return wco; }));
    var Scheduling = klass('Scheduling', null), scheduling = Object.create(Scheduling.prototype);
    method(Scheduling.prototype, 'isInputPending', function () { return false; });
    put('scheduling', getter(function () { return scheduling; }));
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- notifications
  (function () {
    if (g.Notification) return;
    var Notification = klass('Notification', ET, function (title, options) {
      if (arguments.length < 1) throw typeErr("construct 'Notification'", '1 argument required, but only 0 present.');
      options = options || {};
      this.title = String(title); this.body = options.body === undefined ? '' : String(options.body); this.icon = options.icon || ''; this.tag = options.tag || '';
      this.data = options.data === undefined ? null : options.data; this.lang = options.lang || ''; this.dir = options.dir || 'auto'; this.silent = options.silent === undefined ? null : options.silent;
      this.requireInteraction = !!options.requireInteraction; this.timestamp = Date.now();
      this.onclick = this.onclose = this.onerror = this.onshow = null;
      var self = this;
      later(function () { self.dispatchEvent(new Event('error')); });   // (no permission: the notification is not shown)
    });
    Object.defineProperty(Notification, 'permission', { get: function () { return 'denied'; }, enumerable: true });
    Notification.maxActions = 2;
    Notification.requestPermission = function (callback) { if (typeof callback === 'function') later(function () { callback('denied'); }); return Promise.resolve('denied'); };
    method(Notification.prototype, 'close', function () { });
  }());
  if (!g.SharedWorker) {
    var SharedWorker = klass('SharedWorker', ET, function () { throw new DOMException('SharedWorker is not supported by this browser.', 'NotSupportedError'); });
  }

  // ---------------------------------------------------------------------------------------------------------------------------------- caches, cookie store
  (function () {
    if (g.caches) return;
    var CacheStorage = klass('CacheStorage', null), Cache = klass('Cache', null);
    var keyOf = function (request) { return (request instanceof g.Request ? request.url : new URL(String(request), g.location.href).href); };
    var newCache = function () {
      var c = Object.create(Cache.prototype), entries = [];
      hide(c, '__entries', entries);
      return c;
    };
    method(Cache.prototype, 'put', function (request, response) {
      var key = keyOf(request), entries = this.__entries;
      return response.clone().arrayBuffer().then(function (buffer) {
        var stored = { key: key, request: request instanceof g.Request ? request : new g.Request(key), status: response.status, statusText: response.statusText, headers: Array.from ? Array.from(response.headers) : [], body: buffer };
        for (var i = 0; i < entries.length; i++) if (entries[i].key === key) { entries[i] = stored; return; }
        entries.push(stored);
      });
    });
    var rebuild = function (e) { return new g.Response(e.body.slice(0), { status: e.status, statusText: e.statusText, headers: e.headers }); };
    method(Cache.prototype, 'match', function (request) {
      var key = keyOf(request);
      for (var i = 0; i < this.__entries.length; i++) if (this.__entries[i].key === key) return Promise.resolve(rebuild(this.__entries[i]));
      return Promise.resolve(undefined);
    });
    method(Cache.prototype, 'matchAll', function (request) {
      var out = [], key = request === undefined ? null : keyOf(request);
      for (var i = 0; i < this.__entries.length; i++) if (key === null || this.__entries[i].key === key) out.push(rebuild(this.__entries[i]));
      return Promise.resolve(out);
    });
    method(Cache.prototype, 'add', function (request) { var self = this; return g.fetch(request).then(function (r) { if (!r.ok) throw typeErr("execute 'add' on 'Cache'", 'Request failed'); return self.put(request, r); }); });
    method(Cache.prototype, 'addAll', function (requests) { var self = this; return Promise.all(Array.prototype.map.call(requests, function (r) { return self.add(r); })).then(function () { }); });
    method(Cache.prototype, 'delete', function (request) {
      var key = keyOf(request);
      for (var i = 0; i < this.__entries.length; i++) if (this.__entries[i].key === key) { this.__entries.splice(i, 1); return Promise.resolve(true); }
      return Promise.resolve(false);
    });
    method(Cache.prototype, 'keys', function () { return Promise.resolve(this.__entries.map(function (e) { return e.request; })); });
    var all = {}, storage = Object.create(CacheStorage.prototype);
    method(CacheStorage.prototype, 'open', function (name) { name = String(name); return Promise.resolve(all[name] || (all[name] = newCache())); });
    method(CacheStorage.prototype, 'has', function (name) { return Promise.resolve(all[String(name)] !== undefined); });
    method(CacheStorage.prototype, 'delete', function (name) { name = String(name); var had = all[name] !== undefined; delete all[name]; return Promise.resolve(had); });
    method(CacheStorage.prototype, 'keys', function () { return Promise.resolve(Object.keys(all)); });
    method(CacheStorage.prototype, 'match', function (request) {
      var names = Object.keys(all), i = 0;
      var next = function () { if (i >= names.length) return Promise.resolve(undefined); return all[names[i++]].match(request).then(function (r) { return r || next(); }); };
      return next();
    });
    defineGetter(g, 'caches', function () { return storage; });
    // cookieStore, over document.cookie
    var CookieStore = klass('CookieStore', ET), cookieStore = Object.create(CookieStore.prototype);
    cookieStore.onchange = null;
    var cookies = function () {
      var out = [], text = g.document.cookie;
      if (text) text.split('; ').forEach(function (p) { var eq = p.indexOf('='); out.push({ name: eq < 0 ? '' : p.slice(0, eq), value: eq < 0 ? p : p.slice(eq + 1) }); });
      return out;
    };
    var pick = function (arg) { return typeof arg === 'string' ? { name: arg } : arg || {}; };
    method(CookieStore.prototype, 'getAll', function (arg) { var q = pick(arg); return Promise.resolve(cookies().filter(function (c) { return q.name === undefined || c.name === q.name; })); });
    method(CookieStore.prototype, 'get', function (arg) { var q = pick(arg); var m = cookies().filter(function (c) { return q.name === undefined || c.name === q.name; }); return Promise.resolve(m.length ? m[0] : null); });
    method(CookieStore.prototype, 'set', function (a, b) {
      var c = typeof a === 'string' ? { name: a, value: b } : a;
      var text = c.name + '=' + c.value + '; path=' + (c.path || '/');
      if (c.expires !== undefined && c.expires !== null) text += '; expires=' + new Date(c.expires).toUTCString();
      if (c.domain) text += '; domain=' + c.domain;
      if (c.sameSite) text += '; samesite=' + c.sameSite;
      g.document.cookie = text;
      return Promise.resolve();
    });
    method(CookieStore.prototype, 'delete', function (arg) { var q = pick(arg); g.document.cookie = q.name + '=; path=' + (q.path || '/') + '; expires=Thu, 01 Jan 1970 00:00:00 GMT'; return Promise.resolve(); });
    defineGetter(g, 'cookieStore', function () { return cookieStore; });
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- geometry
  (function () {
    var num = function (v) { v = Number(v); return v; };
    var DOMRectReadOnly = klass('DOMRectReadOnly', null, function (x, y, w, h) { hide(this, '__r', [num(x || 0), num(y || 0), num(w || 0), num(h || 0)]); });
    var DOMRect = klass('DOMRect', DOMRectReadOnly, function (x, y, w, h) { hide(this, '__r', [num(x || 0), num(y || 0), num(w || 0), num(h || 0)]); });
    var rectFields = [['x', 0], ['y', 1], ['width', 2], ['height', 3]];
    rectFields.forEach(function (f) {
      accessorOf(DOMRectReadOnly.prototype, f[0], function () { return this.__r[f[1]]; });
      accessorOf(DOMRect.prototype, f[0], function () { return this.__r[f[1]]; }, function (v) { this.__r[f[1]] = num(v); });
    });
    var edges = { top: function (r) { return Math.min(r[1], r[1] + r[3]); }, bottom: function (r) { return Math.max(r[1], r[1] + r[3]); },
                  left: function (r) { return Math.min(r[0], r[0] + r[2]); }, right: function (r) { return Math.max(r[0], r[0] + r[2]); } };
    Object.keys(edges).forEach(function (k) { accessorOf(DOMRectReadOnly.prototype, k, function () { return edges[k](this.__r); }); });
    method(DOMRectReadOnly.prototype, 'toJSON', function () { var r = this.__r; return { x: r[0], y: r[1], width: r[2], height: r[3], top: edges.top(r), right: edges.right(r), bottom: edges.bottom(r), left: edges.left(r) }; });
    DOMRectReadOnly.fromRect = function (o) { o = o || {}; return new DOMRectReadOnly(o.x, o.y, o.width, o.height); };
    DOMRect.fromRect = function (o) { o = o || {}; return new DOMRect(o.x, o.y, o.width, o.height); };
    var DOMPointReadOnly = klass('DOMPointReadOnly', null, function (x, y, z, w) { hide(this, '__p', [num(x || 0), num(y || 0), num(z || 0), w === undefined ? 1 : num(w)]); });
    var DOMPoint = klass('DOMPoint', DOMPointReadOnly, function (x, y, z, w) { hide(this, '__p', [num(x || 0), num(y || 0), num(z || 0), w === undefined ? 1 : num(w)]); });
    ['x', 'y', 'z', 'w'].forEach(function (n, i) {
      accessorOf(DOMPointReadOnly.prototype, n, function () { return this.__p[i]; });
      accessorOf(DOMPoint.prototype, n, function () { return this.__p[i]; }, function (v) { this.__p[i] = num(v); });
    });
    method(DOMPointReadOnly.prototype, 'toJSON', function () { return { x: this.x, y: this.y, z: this.z, w: this.w }; });
    DOMPointReadOnly.fromPoint = function (o) { o = o || {}; return new DOMPointReadOnly(o.x, o.y, o.z, o.w); };
    DOMPoint.fromPoint = function (o) { o = o || {}; return new DOMPoint(o.x, o.y, o.z, o.w); };
    var DOMMatrixReadOnly = klass('DOMMatrixReadOnly', null, function (init) { hide(this, '__m', parseMatrix(init)); });
    var DOMMatrix = klass('DOMMatrix', DOMMatrixReadOnly, function (init) { hide(this, '__m', parseMatrix(init)); });
    function identity() { return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }   // column major: m11 m12 m13 m14 m21 ...
    function parseMatrix(init) {
      var m = identity();
      if (init === undefined || init === null) return m;
      if (typeof init === 'string') {
        init = init.trim();
        if (init === '' || init === 'none') return m;
        var re = /([a-zA-Z0-9]+)\(([^)]*)\)/g, found;
        while ((found = re.exec(init))) {
          var a = found[2].split(',').map(function (x) { return parseFloat(x); }), op = found[1], t = identity();
          if (op === 'matrix' && a.length === 6) { t[0] = a[0]; t[1] = a[1]; t[4] = a[2]; t[5] = a[3]; t[12] = a[4]; t[13] = a[5]; }
          else if (op === 'matrix3d' && a.length === 16) t = a;
          else if (op === 'translate' || op === 'translateX' || op === 'translateY' || op === 'translate3d') { t[12] = op === 'translateY' ? 0 : a[0] || 0; t[13] = op === 'translateY' ? a[0] : op === 'translateX' ? 0 : a[1] || 0; t[14] = op === 'translate3d' ? a[2] || 0 : 0; }
          else if (op === 'scale' || op === 'scaleX' || op === 'scaleY' || op === 'scale3d') { t[0] = op === 'scaleY' ? 1 : a[0]; t[5] = op === 'scaleX' ? 1 : op === 'scaleY' ? a[0] : a.length > 1 ? a[1] : a[0]; t[10] = op === 'scale3d' ? a[2] : 1; }
          else if (op === 'rotate' || op === 'rotateZ') { var r = a[0] * Math.PI / 180; t[0] = Math.cos(r); t[1] = Math.sin(r); t[4] = -Math.sin(r); t[5] = Math.cos(r); }
          else if (op === 'skewX') t[4] = Math.tan(a[0] * Math.PI / 180);
          else if (op === 'skewY') t[1] = Math.tan(a[0] * Math.PI / 180);
          else throw new DOMException("Failed to construct 'DOMMatrix': Failed to parse '" + init + "'.", 'SyntaxError');
          m = mul(m, t);
        }
        return m;
      }
      if (init.length === 6) { m[0] = init[0]; m[1] = init[1]; m[4] = init[2]; m[5] = init[3]; m[12] = init[4]; m[13] = init[5]; return m; }
      if (init.length === 16) return Array.prototype.slice.call(init).map(Number);
      throw typeErr("construct 'DOMMatrix'", 'The sequence must contain 6 elements for a 2D matrix or 16 elements for a 3D matrix.');
    }
    function mul(a, b) {   // a * b (column major)
      var o = new Array(16);
      for (var c = 0; c < 4; c++) for (var r = 0; r < 4; r++) { var s = 0; for (var k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k]; o[c * 4 + r] = s; }
      return o;
    }
    var names3d = ['m11', 'm12', 'm13', 'm14', 'm21', 'm22', 'm23', 'm24', 'm31', 'm32', 'm33', 'm34', 'm41', 'm42', 'm43', 'm44'];
    names3d.forEach(function (n, i) {
      accessorOf(DOMMatrixReadOnly.prototype, n, function () { return this.__m[i]; });
      accessorOf(DOMMatrix.prototype, n, function () { return this.__m[i]; }, function (v) { this.__m[i] = num(v); });
    });
    [['a', 0], ['b', 1], ['c', 4], ['d', 5], ['e', 12], ['f', 13]].forEach(function (p) {
      accessorOf(DOMMatrixReadOnly.prototype, p[0], function () { return this.__m[p[1]]; });
      accessorOf(DOMMatrix.prototype, p[0], function () { return this.__m[p[1]]; }, function (v) { this.__m[p[1]] = num(v); });
    });
    accessorOf(DOMMatrixReadOnly.prototype, 'is2D', function () { var m = this.__m; return m[2] === 0 && m[3] === 0 && m[6] === 0 && m[7] === 0 && m[8] === 0 && m[9] === 0 && m[10] === 1 && m[11] === 0 && m[14] === 0 && m[15] === 1; });
    accessorOf(DOMMatrixReadOnly.prototype, 'isIdentity', function () { var m = this.__m, i = identity(); for (var k = 0; k < 16; k++) if (m[k] !== i[k]) return false; return true; });
    var derive = function (self, m) { var out = Object.create(DOMMatrix.prototype); hide(out, '__m', m); return out; };
    var both = function (name, fn) {
      method(DOMMatrixReadOnly.prototype, name, function () { return derive(this, fn(this.__m.slice(), arguments)); });
      method(DOMMatrix.prototype, name + 'Self', function () { this.__m = fn(this.__m.slice(), arguments); return this; });
    };
    both('multiply', function (m, a) { return mul(m, new DOMMatrix(a[0].__m || a[0]).__m); });
    both('translate', function (m, a) { var t = identity(); t[12] = Number(a[0] || 0); t[13] = Number(a[1] || 0); t[14] = Number(a[2] || 0); return mul(m, t); });
    both('scale', function (m, a) { var t = identity(), sx = a[0] === undefined ? 1 : Number(a[0]), sy = a[1] === undefined ? sx : Number(a[1]), sz = a[2] === undefined ? 1 : Number(a[2]); t[0] = sx; t[5] = sy; t[10] = sz; return mul(m, t); });
    both('rotate', function (m, a) { var deg = a.length === 1 ? Number(a[0]) : Number(a[2] === undefined ? a[0] : a[2]), r = deg * Math.PI / 180, t = identity(); t[0] = Math.cos(r); t[1] = Math.sin(r); t[4] = -Math.sin(r); t[5] = Math.cos(r); return mul(m, t); });
    both('skewX', function (m, a) { var t = identity(); t[4] = Math.tan(Number(a[0] || 0) * Math.PI / 180); return mul(m, t); });
    both('skewY', function (m, a) { var t = identity(); t[1] = Math.tan(Number(a[0] || 0) * Math.PI / 180); return mul(m, t); });
    both('invert', function (m) {   // by Gauss-Jordan; a matrix that cannot be inverted becomes NaN, as the standard says
      var n = 4, a = [], r, c, k;
      for (r = 0; r < n; r++) { a[r] = []; for (c = 0; c < n; c++) a[r][c] = m[c * 4 + r]; for (c = 0; c < n; c++) a[r][n + c] = r === c ? 1 : 0; }
      for (c = 0; c < n; c++) {
        var p = c;
        for (r = c + 1; r < n; r++) if (Math.abs(a[r][c]) > Math.abs(a[p][c])) p = r;
        if (Math.abs(a[p][c]) < 1e-12) { var nan = []; for (k = 0; k < 16; k++) nan.push(NaN); return nan; }
        var tmp = a[c]; a[c] = a[p]; a[p] = tmp;
        var d = a[c][c];
        for (k = 0; k < 2 * n; k++) a[c][k] /= d;
        for (r = 0; r < n; r++) if (r !== c) { var f = a[r][c]; for (k = 0; k < 2 * n; k++) a[r][k] -= f * a[c][k]; }
      }
      var o = new Array(16);
      for (r = 0; r < n; r++) for (c = 0; c < n; c++) o[c * 4 + r] = a[r][n + c];
      return o;
    });
    method(DOMMatrixReadOnly.prototype, 'inverse', DOMMatrixReadOnly.prototype.invert);
    method(DOMMatrix.prototype, 'invertSelf', DOMMatrix.prototype.inverseSelf || DOMMatrix.prototype.invertSelf);
    both('rotateFromVector', function (m, a) { var x = Number(a[0] || 0), y = Number(a[1] || 0), r = x === 0 && y === 0 ? 0 : Math.atan2(y, x), t = identity(); t[0] = Math.cos(r); t[1] = Math.sin(r); t[4] = -Math.sin(r); t[5] = Math.cos(r); return mul(m, t); });
    both('rotateAxisAngle', function (m, a) {
      var x = Number(a[0] || 0), y = Number(a[1] || 0), z = Number(a[2] || 0), len = Math.sqrt(x * x + y * y + z * z), r = Number(a[3] || 0) * Math.PI / 180, t = identity();
      if (len === 0) return m;
      x /= len; y /= len; z /= len;
      var c = Math.cos(r), s2 = Math.sin(r), k = 1 - c;
      t[0] = c + x * x * k; t[1] = y * x * k + z * s2; t[2] = z * x * k - y * s2;
      t[4] = x * y * k - z * s2; t[5] = c + y * y * k; t[6] = z * y * k + x * s2;
      t[8] = x * z * k + y * s2; t[9] = y * z * k - x * s2; t[10] = c + z * z * k;
      return mul(m, t);
    });
    both('scale3d', function (m, a) { var s3 = a[0] === undefined ? 1 : Number(a[0]), t = identity(); t[0] = t[5] = t[10] = s3; return mul(m, t); });
    both('scaleNonUniform', function (m, a) { var t = identity(); t[0] = a[0] === undefined ? 1 : Number(a[0]); t[5] = a[1] === undefined ? 1 : Number(a[1]); return mul(m, t); });
    method(DOMMatrixReadOnly.prototype, 'flipX', function () { return this.multiply(new DOMMatrix([-1, 0, 0, 1, 0, 0])); });
    method(DOMMatrixReadOnly.prototype, 'flipY', function () { return this.multiply(new DOMMatrix([1, 0, 0, -1, 0, 0])); });
    method(DOMMatrixReadOnly.prototype, 'transformPoint', function (p) {
      p = p || {}; var m = this.__m, x = Number(p.x || 0), y = Number(p.y || 0), z = Number(p.z || 0), w = p.w === undefined ? 1 : Number(p.w);
      return new DOMPoint(m[0] * x + m[4] * y + m[8] * z + m[12] * w, m[1] * x + m[5] * y + m[9] * z + m[13] * w, m[2] * x + m[6] * y + m[10] * z + m[14] * w, m[3] * x + m[7] * y + m[11] * z + m[15] * w);
    });
    method(DOMMatrixReadOnly.prototype, 'toFloat32Array', function () { return new Float32Array(this.__m); });
    method(DOMMatrixReadOnly.prototype, 'toFloat64Array', function () { return new Float64Array(this.__m); });
    method(DOMMatrixReadOnly.prototype, 'toJSON', function () { var o = {}, self = this; ['a', 'b', 'c', 'd', 'e', 'f'].concat(names3d).forEach(function (n) { o[n] = self[n]; }); o.is2D = this.is2D; o.isIdentity = this.isIdentity; return o; });
    method(DOMMatrixReadOnly.prototype, 'toString', function () {
      if (!this.is2D) return 'matrix3d(' + this.__m.join(', ') + ')';
      var m = this.__m; return 'matrix(' + [m[0], m[1], m[4], m[5], m[12], m[13]].join(', ') + ')';
    });
    method(DOMMatrix.prototype, 'setMatrixValue', function (v) { this.__m = parseMatrix(v); return this; });
    method(DOMMatrix.prototype, 'preMultiplySelf', function (o) { this.__m = mul(new DOMMatrix(o.__m || o).__m, this.__m); return this; });
    DOMMatrixReadOnly.fromMatrix = function (o) { return new DOMMatrixReadOnly(o && o.__m ? o.__m : o); };
    DOMMatrix.fromMatrix = function (o) { return new DOMMatrix(o && o.__m ? o.__m : o); };
    DOMMatrix.fromFloat32Array = DOMMatrix.fromFloat64Array = function (a) { return new DOMMatrix(a); };
    hide(g, 'WebKitCSSMatrix', DOMMatrix);
    var DOMQuad = klass('DOMQuad', null, function (p1, p2, p3, p4) {
      hide(this, '__q', [DOMPoint.fromPoint(p1), DOMPoint.fromPoint(p2), DOMPoint.fromPoint(p3), DOMPoint.fromPoint(p4)]);
    });
    ['p1', 'p2', 'p3', 'p4'].forEach(function (n, i) { accessorOf(DOMQuad.prototype, n, function () { return this.__q[i]; }); });
    method(DOMQuad.prototype, 'getBounds', function () {
      var xs = this.__q.map(function (p) { return p.x; }), ys = this.__q.map(function (p) { return p.y; });
      var x = Math.min.apply(null, xs), y = Math.min.apply(null, ys);
      return new DOMRect(x, y, Math.max.apply(null, xs) - x, Math.max.apply(null, ys) - y);
    });
    DOMQuad.fromRect = function (r) { r = r || {}; var x = r.x || 0, y = r.y || 0, w = r.width || 0, h = r.height || 0; return new DOMQuad({ x: x, y: y }, { x: x + w, y: y }, { x: x + w, y: y + h }, { x: x, y: y + h }); };
    // getBoundingClientRect and getClientRects return rectangles of this kind
    var plainRect = EP.getBoundingClientRect;
    if (plainRect) {
      method(EP, 'getBoundingClientRect', function () { var r = plainRect.call(this); return new DOMRect(r.x, r.y, r.width, r.height); });
      var plainRects = EP.getClientRects;
      if (plainRects) method(EP, 'getClientRects', function () { var l = plainRects.call(this); var out = []; for (var i = 0; i < l.length; i++) out.push(new DOMRect(l[i].x, l[i].y, l[i].width, l[i].height)); return out; });
    }
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- more events
  var mkEvent = function (name, parent, defaults) {
    var fields = Object.keys(defaults), base = parent || g.Event;
    var C = function (type, init) {
      if (!(this instanceof C)) throw typeErr("construct '" + name + "'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      if (arguments.length < 1) throw typeErr("construct '" + name + "'", '1 argument required, but only 0 present.');
      var e = new base(type, init);
      Object.setPrototypeOf(e, C.prototype);
      for (var i = 0; i < fields.length; i++) {
        var v = init && init[fields[i]] !== undefined ? init[fields[i]] : defaults[fields[i]];
        Object.defineProperty(e, fields[i], { value: v, enumerable: true, writable: true, configurable: true });
      }
      return e;
    };
    Object.defineProperty(C, 'name', { value: name });
    C.prototype = Object.create(base.prototype);
    Object.defineProperty(C.prototype, 'constructor', { value: C, writable: true, configurable: true });
    tag(C, name);
    hide(g, name, C);
    return C;
  };
  if (!g.DragEvent) mkEvent('DragEvent', g.MouseEvent, { dataTransfer: null });
  if (!g.ClipboardEvent) mkEvent('ClipboardEvent', null, { clipboardData: null });
  if (!g.PageTransitionEvent) mkEvent('PageTransitionEvent', null, { persisted: false });
  if (!g.BeforeUnloadEvent) mkEvent('BeforeUnloadEvent', null, { returnValue: '' });
  if (!g.SubmitEvent) mkEvent('SubmitEvent', null, { submitter: null });
  if (!g.FormDataEvent) mkEvent('FormDataEvent', null, { formData: null });
  if (!g.ToggleEvent) mkEvent('ToggleEvent', null, { oldState: '', newState: '' });
  if (!g.SecurityPolicyViolationEvent) mkEvent('SecurityPolicyViolationEvent', null, { documentURI: '', referrer: '', blockedURI: '', violatedDirective: '', effectiveDirective: '', originalPolicy: '', disposition: 'enforce', statusCode: 0 });
  // the old init methods
  var legacy = function (proto, name, fields) {
    if (!proto || proto[name]) return;
    method(proto, name, function () {
      var e = this;
      if (e.__dispatching) return;
      var a = arguments;
      if (typeof a[0] !== 'undefined') Object.defineProperty(e, 'type', { value: String(a[0]), configurable: true });
      for (var i = 1; i < fields.length; i++) {
        if (fields[i] === null || a[i] === undefined) continue;
        Object.defineProperty(e, fields[i], { value: fields[i] === 'bubbles' || fields[i] === 'cancelable' || fields[i].indexOf('Key') >= 0 && typeof a[i] === 'boolean' ? !!a[i] : a[i], configurable: true, enumerable: true, writable: true });
      }
    });
  };
  legacy(g.UIEvent && g.UIEvent.prototype, 'initUIEvent', ['type', 'bubbles', 'cancelable', 'view', 'detail']);
  legacy(g.MouseEvent && g.MouseEvent.prototype, 'initMouseEvent', ['type', 'bubbles', 'cancelable', 'view', 'detail', 'screenX', 'screenY', 'clientX', 'clientY', 'ctrlKey', 'altKey', 'shiftKey', 'metaKey', 'button', 'relatedTarget']);
  legacy(g.KeyboardEvent && g.KeyboardEvent.prototype, 'initKeyboardEvent', ['type', 'bubbles', 'cancelable', 'view', 'key', 'location', 'ctrlKey', 'altKey', 'shiftKey', 'metaKey']);
  legacy(g.CustomEvent && g.CustomEvent.prototype, 'initCustomEvent', ['type', 'bubbles', 'cancelable', 'detail']);
  legacy(g.FocusEvent && g.FocusEvent.prototype, 'initFocusEvent', ['type', 'bubbles', 'cancelable', 'view', 'detail', 'relatedTarget']);
  legacy(g.CompositionEvent && g.CompositionEvent.prototype, 'initCompositionEvent', ['type', 'bubbles', 'cancelable', 'view', 'data']);
  legacy(g.StorageEvent && g.StorageEvent.prototype, 'initStorageEvent', ['type', 'bubbles', 'cancelable', 'key', 'oldValue', 'newValue', 'url', 'storageArea']);

  // ---------------------------------------------------------------------------------------------------------------------------------- data transfer
  (function () {
    var DataTransferItem = klass('DataTransferItem', null), DataTransferItemList = klass('DataTransferItemList', null);
    var DataTransfer = klass('DataTransfer', null, function () {
      hide(this, '__d', { data: {}, order: [], files: [], items: null });
      this.dropEffect = 'none'; this.effectAllowed = 'uninitialized';
    });
    var fmt = function (f) { f = String(f).toLowerCase(); return f === 'text' ? 'text/plain' : f === 'url' ? 'text/uri-list' : f; };
    method(DataTransfer.prototype, 'setData', function (format, data) {
      if (arguments.length < 2) throw typeErr("execute 'setData' on 'DataTransfer'", '2 arguments required, but only ' + arguments.length + ' present.');
      format = fmt(format);
      if (this.__d.data[format] === undefined) this.__d.order.push(format);
      this.__d.data[format] = String(data);
    });
    method(DataTransfer.prototype, 'getData', function (format) { var v = this.__d.data[fmt(format)]; return v === undefined ? '' : v; });
    method(DataTransfer.prototype, 'clearData', function (format) {
      var d = this.__d;
      if (format === undefined) { d.data = {}; d.order = []; return; }
      format = fmt(format);
      if (d.data[format] !== undefined) { delete d.data[format]; d.order.splice(d.order.indexOf(format), 1); }
    });
    method(DataTransfer.prototype, 'setDragImage', function () { });
    defineGetter(DataTransfer.prototype, 'types', function () { return this.__d.order.slice().concat(this.__d.files.length ? ['Files'] : []); });
    defineGetter(DataTransfer.prototype, 'files', function () { return this.__d.files; });
    defineGetter(DataTransfer.prototype, 'items', function () {
      var dt = this, list = Object.create(DataTransferItemList.prototype);
      hide(list, '__dt', dt);
      return list;
    });
    defineGetter(DataTransferItemList.prototype, 'length', function () { return this.__dt.__d.order.length + this.__dt.__d.files.length; });
    method(DataTransferItemList.prototype, 'add', function (data, type) {
      var dt = this.__dt;
      if (typeof data === 'string') { dt.setData(type, data); return this.item(dt.__d.order.length - 1); }
      dt.__d.files.push(data);
      return null;
    });
    method(DataTransferItemList.prototype, 'clear', function () { this.__dt.clearData(); this.__dt.__d.files = []; });
    method(DataTransferItemList.prototype, 'remove', function (i) { var d = this.__dt.__d; if (i < d.order.length) this.__dt.clearData(d.order[i]); else d.files.splice(i - d.order.length, 1); });
    method(DataTransferItemList.prototype, 'item', function (i) {
      var dt = this.__dt, d = dt.__d, item = Object.create(DataTransferItem.prototype);
      if (i < d.order.length) { Object.defineProperty(item, 'kind', { value: 'string' }); Object.defineProperty(item, 'type', { value: d.order[i] }); hide(item, '__s', d.data[d.order[i]]); }
      else if (i - d.order.length < d.files.length) { Object.defineProperty(item, 'kind', { value: 'file' }); Object.defineProperty(item, 'type', { value: d.files[i - d.order.length].type }); hide(item, '__f', d.files[i - d.order.length]); }
      else return undefined;
      return item;
    });
    method(DataTransferItem.prototype, 'getAsString', function (cb) { var s = this.__s; if (typeof cb === 'function' && s !== undefined) later(function () { cb(s); }); });
    method(DataTransferItem.prototype, 'getAsFile', function () { return this.__f || null; });
    method(DataTransferItem.prototype, 'webkitGetAsEntry', function () { return null; });
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- performance
  (function () {
    var P = g.performance;
    if (!P) return;
    var Performance = klass('Performance', ET), PerformanceEntry = klass('PerformanceEntry', null);
    var PerformanceMark = klass('PerformanceMark', PerformanceEntry, function (name, options) {
      options = options || {};
      Object.defineProperty(this, 'name', { value: String(name), enumerable: true }); Object.defineProperty(this, 'entryType', { value: 'mark', enumerable: true });
      Object.defineProperty(this, 'startTime', { value: options.startTime === undefined ? P.now() : Number(options.startTime), enumerable: true });
      Object.defineProperty(this, 'duration', { value: 0, enumerable: true }); Object.defineProperty(this, 'detail', { value: options.detail === undefined ? null : options.detail, enumerable: true });
    });
    var PerformanceMeasure = klass('PerformanceMeasure', PerformanceEntry);
    var PerformanceResourceTiming = klass('PerformanceResourceTiming', PerformanceEntry), PerformanceNavigationTiming = klass('PerformanceNavigationTiming', PerformanceResourceTiming);
    var PerformancePaintTiming = klass('PerformancePaintTiming', PerformanceEntry);
    method(PerformanceEntry.prototype, 'toJSON', function () { return { name: this.name, entryType: this.entryType, startTime: this.startTime, duration: this.duration }; });
    try { Object.setPrototypeOf(P, Performance.prototype); } catch (e) { }
    var entries = [], origin = Date.now() - P.now();
    var entry = function (C, name, type, start, duration, detail) {
      var e = Object.create(C.prototype);
      Object.defineProperty(e, 'name', { value: name, enumerable: true }); Object.defineProperty(e, 'entryType', { value: type, enumerable: true });
      Object.defineProperty(e, 'startTime', { value: start, enumerable: true }); Object.defineProperty(e, 'duration', { value: duration, enumerable: true });
      if (detail !== undefined) Object.defineProperty(e, 'detail', { value: detail, enumerable: true });
      return e;
    };
    var navigation = entry(PerformanceNavigationTiming, g.location.href, 'navigation', 0, P.now(), undefined);
    [['type', 'navigate'], ['redirectCount', 0], ['initiatorType', 'navigation'], ['nextHopProtocol', 'http/1.1'], ['transferSize', 0], ['encodedBodySize', 0], ['decodedBodySize', 0],
     ['domContentLoadedEventStart', 0], ['domContentLoadedEventEnd', 0], ['domInteractive', 0], ['domComplete', 0], ['loadEventStart', 0], ['loadEventEnd', 0], ['fetchStart', 0], ['responseEnd', 0]].forEach(function (p) {
      Object.defineProperty(navigation, p[0], { value: p[1], enumerable: true });
    });
    var paints = [entry(PerformancePaintTiming, 'first-paint', 'paint', 1, 0), entry(PerformancePaintTiming, 'first-contentful-paint', 'paint', 1, 0)];
    var add = function (e) {
      entries.push(e);
      return e;
    };
    var time = function (v, what) {
      if (typeof v === 'number') return v;
      if (v === undefined) return P.now();
      var m = null;
      for (var i = entries.length - 1; i >= 0; i--) if (entries[i].entryType === 'mark' && entries[i].name === String(v)) { m = entries[i]; break; }
      if (!m) throw new DOMException("Failed to execute 'measure' on 'Performance': The mark '" + v + "' does not exist.", 'SyntaxError');
      return m.startTime;
    };
    method(Performance.prototype, 'mark', function (name, options) {
      if (arguments.length < 1) throw typeErr("execute 'mark' on 'Performance'", '1 argument required, but only 0 present.');
      if (options && options.startTime !== undefined && (typeof options.startTime !== 'number' || options.startTime < 0)) throw typeErr("execute 'mark' on 'Performance'", "'startTime' must be a non-negative number.");
      return add(new PerformanceMark(name, options));
    });
    method(Performance.prototype, 'measure', function (name, a, b) {
      if (arguments.length < 1) throw typeErr("execute 'measure' on 'Performance'", '1 argument required, but only 0 present.');
      var start, end, detail;
      if (a !== null && typeof a === 'object') {
        start = a.start === undefined ? (a.end !== undefined && a.duration !== undefined ? time(a.end) - a.duration : 0) : time(a.start);
        end = a.end === undefined ? (a.duration !== undefined && a.start !== undefined ? start + a.duration : P.now()) : time(a.end);
        detail = a.detail;
      } else { start = a === undefined ? 0 : time(a); end = b === undefined ? P.now() : time(b); }
      return add(entry(PerformanceMeasure, String(name), 'measure', start, end - start, detail === undefined ? null : detail));
    });
    var filter = function (pred) { var out = entries.filter(pred); return out.sort(function (x, y) { return x.startTime - y.startTime; }); };
    var all = function () { return [navigation].concat(paints, entries); };
    method(Performance.prototype, 'getEntries', function () { return all().sort(function (x, y) { return x.startTime - y.startTime; }); });
    method(Performance.prototype, 'getEntriesByType', function (type) { type = String(type); return all().filter(function (e) { return e.entryType === type; }).sort(function (x, y) { return x.startTime - y.startTime; }); });
    method(Performance.prototype, 'getEntriesByName', function (name, type) { name = String(name); return all().filter(function (e) { return e.name === name && (type === undefined || e.entryType === String(type)); }).sort(function (x, y) { return x.startTime - y.startTime; }); });
    var clear = function (type) { return function (name) { entries = entries.filter(function (e) { return e.entryType !== type || (name !== undefined && e.name !== String(name)); }); }; };
    method(Performance.prototype, 'clearMarks', clear('mark'));
    method(Performance.prototype, 'clearMeasures', clear('measure'));
    method(Performance.prototype, 'clearResourceTimings', function () { });
    method(Performance.prototype, 'setResourceTimingBufferSize', function () { });
    method(Performance.prototype, 'toJSON', function () { return { timeOrigin: P.timeOrigin, timing: P.timing, navigation: P.navigation }; });
    method(Performance.prototype, 'now', P.now);
    defineGetter(Performance.prototype, 'timeOrigin', function () { return origin; });
    defineGetter(Performance.prototype, 'memory', function () { return { usedJSHeapSize: 0, totalJSHeapSize: 0, jsHeapSizeLimit: 0 }; });
    defineGetter(Performance.prototype, 'eventCounts', function () { return new Map(); });
    [['timing', P.timing], ['navigation', P.navigation]].forEach(function (p) { if (p[1]) defineGetter(Performance.prototype, p[0], function () { return p[1]; }); });
    ['mark', 'measure', 'getEntriesByName', 'getEntriesByType', 'now'].forEach(function (k) { try { delete P[k]; } catch (e) { } });
    hide(g, 'PerformanceObserverEntryList', klass('PerformanceObserverEntryList', null));
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- document
  (function () {
    var D = DP;
    var put = function (name, get, set) { if (!(name in doc)) Object.defineProperty(D, name, { get: get, set: set, enumerable: true, configurable: true }); };
    var emptyList = function (C) { return function () { return Object.create(C.prototype); }; };
    put('plugins', function () { return g.navigator.plugins; });
    put('applets', function () { return []; });
    put('all', function () { var l = Array.prototype.slice.call(this.getElementsByTagName('*')); l.namedItem = function (n) { return doc.getElementById(n) || doc.getElementsByName(n)[0] || null; }; return l; });
    put('wasDiscarded', function () { return false; });
    put('pointerLockElement', function () { return null; });
    put('xmlVersion', function () { return this.contentType === 'text/html' ? null : '1.0'; }, function () { });
    put('xmlEncoding', function () { return null; });
    put('xmlStandalone', function () { return false; }, function () { });
    var colors = { fgColor: 'text', linkColor: 'link', vlinkColor: 'vlink', alinkColor: 'alink', bgColor: 'bgcolor' };
    Object.keys(colors).forEach(function (k) {
      put(k, function () { return this.body ? (this.body.getAttribute(colors[k]) || '') : ''; }, function (v) { if (this.body) this.body.setAttribute(colors[k], String(v)); });
    });
    var m = function (name, fn) { if (!(name in doc)) method(D, name, fn); };
    m('clear', function () { });
    m('queryCommandValue', function () { return ''; });
    m('queryCommandIndeterm', function () { return false; });
    m('caretPositionFromPoint', function () { return null; });
    m('caretRangeFromPoint', function () { return null; });
    m('getAnimations', function () { return []; });
    m('requestStorageAccess', function () { return Promise.resolve(); });
    m('hasStorageAccess', function () { return Promise.resolve(true); });
    m('exitPointerLock', function () { });
    m('moveBefore', function (node, child) { return this.insertBefore(node, child); });
    var Policy = klass('PermissionsPolicy', null), policy = Object.create(Policy.prototype);
    method(Policy.prototype, 'allowsFeature', function () { return true; });
    method(Policy.prototype, 'features', function () { return []; });
    method(Policy.prototype, 'allowedFeatures', function () { return []; });
    method(Policy.prototype, 'getAllowlistForFeature', function () { return ['*']; });
    put('permissionsPolicy', function () { return policy; });
    put('featurePolicy', function () { return policy; });
    // the handler properties of the document and the window that were not there
    var handlers = ('onabort onanimationcancel onanimationend onanimationiteration onanimationstart onauxclick onbeforeinput onbeforematch onbeforetoggle onblur oncancel oncanplay oncanplaythrough onchange onclick onclose oncontextlost oncontextmenu oncontextrestored oncopy oncuechange oncut ondblclick ondrag ondragend ondragenter ondragleave ondragover ondragstart ondrop ondurationchange onemptied onended onerror onfocus onformdata ongotpointercapture oninput oninvalid onkeydown onkeypress onkeyup onload onloadeddata onloadedmetadata onloadstart onlostpointercapture onmousedown onmouseenter onmouseleave onmousemove onmouseout onmouseover onmouseup onpaste onpause onplay onplaying onpointercancel onpointerdown onpointerenter onpointerleave onpointermove onpointerout onpointerover onpointerrawupdate onpointerup onprogress onratechange onreset onresize onscroll onscrollend onsecuritypolicyviolation onseeked onseeking onselect onselectionchange onselectstart onslotchange onstalled onsubmit onsuspend ontimeupdate ontoggle ontouchcancel ontouchend ontouchmove ontouchstart ontransitioncancel ontransitionend ontransitionrun ontransitionstart onvolumechange onwaiting onwebkitanimationend onwebkitanimationiteration onwebkitanimationstart onwebkittransitionend onwheel').split(' ');
    var docOnly = 'onfullscreenchange onfullscreenerror onpointerlockchange onpointerlockerror onreadystatechange onvisibilitychange onfreeze onresume onprerenderingchange'.split(' ');
    handlers.concat(docOnly).forEach(function (n) { if (doc[n] === undefined) doc[n] = null; });
    var windowOnly = 'onafterprint onbeforeprint onbeforeunload onblur onerror onfocus onhashchange onlanguagechange onmessage onmessageerror onoffline ononline onpagehide onpageshow onpopstate onrejectionhandled onstorage onunhandledrejection onunload ongamepadconnected ongamepaddisconnected ondevicemotion ondeviceorientation ondeviceorientationabsolute onappinstalled onbeforeinstallprompt onpageswap onpagereveal'.split(' ');
    handlers.concat(windowOnly).forEach(function (n) { if (g[n] === undefined) g[n] = null; });
    handlers.forEach(function (n) { if (!(n in EP)) Object.defineProperty(EP, n, { value: null, writable: true }); });
    // createProcessingInstruction / createCDATASection: nodes of those types, over the comment and text nodes of the tree
    var CD = g.CharacterData, PIC = g.ProcessingInstruction, CDATA = g.CDATASection;
    var isName = function (s) { return /^[A-Za-z_:\u0080-￿][A-Za-z0-9_:.\-\u0080-￿]*$/.test(s); };
    m('createProcessingInstruction', function (target, data) {
      if (arguments.length < 2) throw typeErr("execute 'createProcessingInstruction' on 'Document'", '2 arguments required, but only ' + arguments.length + ' present.');
      target = String(target); data = String(data);
      if (!isName(target)) throw new DOMException("Failed to execute 'createProcessingInstruction' on 'Document': The target provided ('" + target + "') is not a valid name.", 'InvalidCharacterError');
      if (data.indexOf('?>') >= 0) throw new DOMException("Failed to execute 'createProcessingInstruction' on 'Document': The data provided ('?>') contains '?>'", 'InvalidCharacterError');
      var node = __make_node(this, 7, target, data);
      Object.setPrototypeOf(node, PIC.prototype);
      return node;
    });
    m('createCDATASection', function (data) {
      if (arguments.length < 1) throw typeErr("execute 'createCDATASection' on 'Document'", '1 argument required, but only 0 present.');
      if (this.contentType === 'text/html') throw new DOMException("Failed to execute 'createCDATASection' on 'Document': This operation is not supported for HTML documents.", 'NotSupportedError');
      data = String(data);
      if (data.indexOf(']]>') >= 0) throw new DOMException("Failed to execute 'createCDATASection' on 'Document': String contains the CDATA closing delimiter ']]>'", 'InvalidStateError');
      var node = __make_node(this, 4, '', data);
      Object.setPrototypeOf(node, CDATA.prototype);
      return node;
    });
    m('getElementsByTagNameNS', function (ns, name) {
      var all = this.getElementsByTagName('*'), out = [];
      for (var i = 0; i < all.length; i++) if ((name === '*' || all[i].localName === name) && (ns === '*' || (ns === null ? !all[i].namespaceURI : all[i].namespaceURI === ns))) out.push(all[i]);
      return out;
    });
    // Symbol.toStringTag of the interfaces that are one object
    var tags = [[g.Window && g.Window.prototype, 'Window'], [g.Document && g.Document.prototype, 'Document'], [g.Location && g.Location.prototype, 'Location'], [g.History && g.History.prototype, 'History']];
    tags.forEach(function (t) { if (t[0] && t[0] !== Object.prototype) try { Object.defineProperty(t[0], Symbol.toStringTag, { value: t[1], configurable: true }); } catch (e) { } });
    try { Object.defineProperty(g, Symbol.toStringTag, { value: 'Window', configurable: true }); } catch (e) { }
    ['localStorage', 'sessionStorage'].forEach(function (n) { try { Object.defineProperty(Object.getPrototypeOf(g[n]), Symbol.toStringTag, { value: 'Storage', configurable: true }); } catch (e) { } });
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- element
  (function () {
    var put = function (name, get, set) { if (!(name in EP)) Object.defineProperty(EP, name, { get: get, set: set, enumerable: true, configurable: true }); };
    var reflectString = function (prop, attr) { put(prop, function () { var v = this.getAttribute(attr); return v === null ? '' : v; }, function (v) { if (v === null) this.removeAttribute(attr); else this.setAttribute(attr, String(v)); }); };
    var reflectBool = function (prop, attr) { put(prop, function () { return this.hasAttribute(attr); }, function (v) { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); }); };
    reflectBool('inert', 'inert');
    reflectString('autocapitalize', 'autocapitalize');
    reflectString('elementTiming', 'elementtiming');
    reflectString('role', 'role');
    put('translate', function () { var v = this.getAttribute('translate'); return v === null || v === '' || v === 'yes'; }, function (v) { this.setAttribute('translate', v ? 'yes' : 'no'); });
    ('Atomic AutoComplete Busy Checked ColCount ColIndex ColSpan Current Description Disabled Expanded HasPopup Hidden Invalid KeyShortcuts Label Level Live Modal MultiLine MultiSelectable Orientation Placeholder PosInSet Pressed ReadOnly Relevant Required RoleDescription RowCount RowIndex RowSpan Selected SetSize Sort ValueMax ValueMin ValueNow ValueText').split(' ').forEach(function (n) {
      reflectString('aria' + n, 'aria-' + n.toLowerCase());
    });
    // part: a list of tokens over the attribute
    var PartList = klass('DOMTokenList', null);
    var tokens = function (el) { var v = el.getAttribute('part'); return v ? v.split(/\s+/).filter(Boolean) : []; };
    put('part', function () {
      var el = this, list = Object.create(PartList.prototype);
      defineGetter(list, 'length', function () { return tokens(el).length; });
      defineGetter(list, 'value', function () { return tokens(el).join(' '); }, function (v) { el.setAttribute('part', String(v)); });
      list.item = function (i) { var t = tokens(el); return i < t.length ? t[i] : null; };
      list.contains = function (t) { return tokens(el).indexOf(String(t)) >= 0; };
      list.add = function () { var t = tokens(el); for (var i = 0; i < arguments.length; i++) if (t.indexOf(String(arguments[i])) < 0) t.push(String(arguments[i])); el.setAttribute('part', t.join(' ')); };
      list.remove = function () { var t = tokens(el), r = Array.prototype.map.call(arguments, String); el.setAttribute('part', t.filter(function (x) { return r.indexOf(x) < 0; }).join(' ')); };
      list.toggle = function (t, force) { t = String(t); var has = list.contains(t); if (force === undefined ? has : !force) { list.remove(t); return false; } list.add(t); return true; };
      list.replace = function (a, b) { if (!list.contains(a)) return false; el.setAttribute('part', tokens(el).map(function (x) { return x === String(a) ? String(b) : x; }).join(' ')); return true; };
      list.toString = function () { return tokens(el).join(' '); };
      return list;
    });
    var m = function (name, fn) { if (!(name in EP)) method(EP, name, fn); };
    m('scroll', function () { return this.scrollTo.apply(this, arguments); });
    m('scrollIntoViewIfNeeded', function () { return this.scrollIntoView(); });
    m('setHTMLUnsafe', function (html) { this.innerHTML = String(html); });
    m('getHTML', function () { return this.innerHTML; });
    m('computedStyleMap', function () {
      var cs = g.getComputedStyle(this), map = {};
      var wrap = function (v) { return { value: v, toString: function () { return String(v); } }; };
      map.get = function (p) { var v = cs.getPropertyValue(p); return v === '' ? undefined : wrap(v); };
      map.has = function (p) { return cs.getPropertyValue(p) !== ''; };
      map.getAll = function (p) { var v = map.get(p); return v ? [v] : []; };
      defineGetter(map, 'size', function () { return cs.length; });
      map.forEach = function (fn) { for (var i = 0; i < cs.length; i++) fn(wrap(cs.getPropertyValue(cs[i])), cs[i], map); };
      return map;
    });
    // popover: the state and the events (the page decides how it shows)
    var open = [];
    put('popover', function () { var v = this.getAttribute('popover'); return v === null ? null : v === 'manual' ? 'manual' : v === 'hint' ? 'hint' : 'auto'; }, function (v) { if (v === null) this.removeAttribute('popover'); else this.setAttribute('popover', String(v)); });
    put('popoverTargetElement', function () { return this.__popoverTarget || null; }, function (v) { Object.defineProperty(this, '__popoverTarget', { value: v, writable: true, configurable: true }); });
    var fireToggle = function (el, type, oldState, newState, cancelable) {
      var e = new g.ToggleEvent(type, { oldState: oldState, newState: newState, cancelable: cancelable });
      el.dispatchEvent(e);
      return !e.defaultPrevented;
    };
    var popoverCheck = function (el, op) {
      if (el.getAttribute('popover') === null) throw new DOMException("Failed to execute '" + op + "' on 'HTMLElement': The element has no popover attribute defined.", 'NotSupportedError');
      if (!el.isConnected) throw new DOMException("Failed to execute '" + op + "' on 'HTMLElement': Invalid on disconnected popover elements.", 'InvalidStateError');
    };
    m('showPopover', function () {
      popoverCheck(this, 'showPopover');
      if (open.indexOf(this) >= 0) throw new DOMException("Failed to execute 'showPopover' on 'HTMLElement': Invalid on popover elements which are already showing.", 'InvalidStateError');
      if (!fireToggle(this, 'beforetoggle', 'closed', 'open', true)) return;
      if (this.popover === 'auto') open.slice().forEach(function (o) { if (o.popover === 'auto' && !o.contains(this)) o.hidePopover(); }, this);
      open.push(this);
      var el = this;
      later(function () { fireToggle(el, 'toggle', 'closed', 'open', false); });
    });
    m('hidePopover', function () {
      popoverCheck(this, 'hidePopover');
      var at = open.indexOf(this);
      if (at < 0) throw new DOMException("Failed to execute 'hidePopover' on 'HTMLElement': Invalid on popover elements which are not showing.", 'InvalidStateError');
      fireToggle(this, 'beforetoggle', 'open', 'closed', false);
      open.splice(at, 1);
      var el = this;
      later(function () { fireToggle(el, 'toggle', 'open', 'closed', false); });
    });
    m('togglePopover', function (force) {
      popoverCheck(this, 'togglePopover');
      var shown = open.indexOf(this) >= 0;
      if (force === undefined ? shown : !force) { if (shown) this.hidePopover(); return false; }
      if (!shown) this.showPopover();
      return true;
    });
    var nativeMatches = EP.matches;
    method(EP, 'matches', function (sel) {
      if (typeof sel === 'string' && sel.indexOf(':popover-open') >= 0) {
        var el = this;
        if (sel.trim() === ':popover-open') return open.indexOf(el) >= 0;
      }
      return nativeMatches.apply(this, arguments);
    });
  }());

  // The classes whose objects the engine hands out as plain arrays: instanceof and their names are right for them.
  (function () {
    var Collection = function (name, extra) {
      var f = function () { throw new TypeError('Illegal constructor'); };
      Object.defineProperty(f, 'name', { value: name });
      f.prototype = Object.create(Array.prototype);
      Object.defineProperty(f.prototype, 'constructor', { value: f, writable: true, configurable: true });
      tag(f, name);
      if (extra) extra(f.prototype);
      hide(g, name, f);
      return f;
    };
    Collection('NodeList', function (p) { method(p, 'item', function (i) { return this[i >>> 0] || null; }); });
    Collection('HTMLCollection', function (p) {
      method(p, 'item', function (i) { return this[i >>> 0] || null; });
      method(p, 'namedItem', function (name) { name = String(name); for (var i = 0; i < this.length; i++) if (this[i].id === name || (this[i].getAttribute && this[i].getAttribute('name') === name)) return this[i]; return null; });
    });
    Collection('NamedNodeMap', function (p) {
      method(p, 'item', function (i) { return this[i >>> 0] || null; });
      method(p, 'getNamedItem', function (n) { n = String(n).toLowerCase(); for (var i = 0; i < this.length; i++) if (this[i].name.toLowerCase() === n) return this[i]; return null; });
    });
    Collection('FileList', function (p) { method(p, 'item', function (i) { return this[i >>> 0] || null; }); });
    Collection('HTMLAllCollection', function (p) { method(p, 'item', function (i) { return this[i >>> 0] || null; }); });
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- CSSOM
  // Style sheets for script: document.styleSheets, element.sheet, CSSStyleSheet (also constructed, with adoptedStyleSheets), the rule classes, rule.style.
  // A sheet is a tree of rules read from the text of its <style>; a change is written back as text, which is what styles the page again.
  (function () {
    var StyleSheet = klass('StyleSheet', null), CSSStyleSheet = klass('CSSStyleSheet', StyleSheet, function (options) {
      initSheet(this, null, options || {});
      this.__constructed = true;
    });
    var CSSRuleList = klass('CSSRuleList', null), CSSRule = klass('CSSRule', null), CSSStyleDeclaration2 = klass('CSSStyleDeclaration', null);
    var MediaList = klass('MediaList', null), StyleSheetList = klass('StyleSheetList', null);
    var kinds = { style: 1, charset: 2, import: 3, media: 4, 'font-face': 5, page: 6, keyframes: 7, keyframe: 8, namespace: 10, supports: 12, 'font-feature-values': 14, layer: 0, container: 0, property: 0, 'counter-style': 11, scope: 0, starting: 0 };
    var ruleClasses = {};
    var defineRule = function (name, parent) { var C = klass(name, parent || CSSRule); ruleClasses[name] = C; return C; };
    var CSSStyleRule = defineRule('CSSStyleRule'), CSSImportRule = defineRule('CSSImportRule'), CSSGroupingRule = defineRule('CSSGroupingRule'), CSSMediaRule = defineRule('CSSMediaRule', CSSGroupingRule);
    var CSSSupportsRule = defineRule('CSSSupportsRule', CSSGroupingRule), CSSContainerRule = defineRule('CSSContainerRule', CSSGroupingRule), CSSLayerBlockRule = defineRule('CSSLayerBlockRule', CSSGroupingRule);
    var CSSLayerStatementRule = defineRule('CSSLayerStatementRule'), CSSFontFaceRule = defineRule('CSSFontFaceRule'), CSSPageRule = defineRule('CSSPageRule', CSSGroupingRule);
    var CSSKeyframesRule = defineRule('CSSKeyframesRule'), CSSKeyframeRule = defineRule('CSSKeyframeRule'), CSSNamespaceRule = defineRule('CSSNamespaceRule'), CSSPropertyRule = defineRule('CSSPropertyRule');
    var CSSCounterStyleRule = defineRule('CSSCounterStyleRule'), CSSScopeRule = defineRule('CSSScopeRule', CSSGroupingRule), CSSStartingStyleRule = defineRule('CSSStartingStyleRule', CSSGroupingRule);
    CSSRule.STYLE_RULE = 1; CSSRule.CHARSET_RULE = 2; CSSRule.IMPORT_RULE = 3; CSSRule.MEDIA_RULE = 4; CSSRule.FONT_FACE_RULE = 5; CSSRule.PAGE_RULE = 6; CSSRule.KEYFRAMES_RULE = 7;
    CSSRule.KEYFRAME_RULE = 8; CSSRule.NAMESPACE_RULE = 10; CSSRule.COUNTER_STYLE_RULE = 11; CSSRule.SUPPORTS_RULE = 12; CSSRule.FONT_FEATURE_VALUES_RULE = 14;
    for (var k in CSSRule) if (/_RULE$/.test(k)) CSSRule.prototype[k] = CSSRule[k];

    // --- reading
    var skipString = function (t, i) { var q = t.charAt(i); i++; while (i < t.length && t.charAt(i) !== q) { if (t.charAt(i) === '\\') i++; i++; } return i + 1; };
    var stripComments = function (t) {
      var out = '', i = 0;
      while (i < t.length) {
        var c = t.charAt(i);
        if (c === '/' && t.charAt(i + 1) === '*') { var e = t.indexOf('*/', i + 2); i = e < 0 ? t.length : e + 2; out += ' '; }
        else if (c === '"' || c === "'") { var j = skipString(t, i); out += t.slice(i, j); i = j; }
        else { out += c; i++; }
      }
      return out;
    };
    // The index of the '}' that closes the '{' at `open`.
    var matching = function (t, open) {
      var depth = 0;
      for (var i = open; i < t.length; i++) {
        var c = t.charAt(i);
        if (c === '"' || c === "'") { i = skipString(t, i) - 1; continue; }
        if (c === '(') { var d = 1; for (i++; i < t.length && d > 0; i++) { var cc = t.charAt(i); if (cc === '"' || cc === "'") i = skipString(t, i) - 1; else if (cc === '(') d++; else if (cc === ')') d--; } i--; continue; }
        if (c === '{') depth++;
        else if (c === '}') { depth--; if (depth === 0) return i; }
      }
      return t.length;
    };
    // The next statement of a list: up to ';' or a block. { prelude, body (null for a statement), end }
    var nextStatement = function (t, i) {
      var start = i;
      while (i < t.length) {
        var c = t.charAt(i);
        if (c === '"' || c === "'") { i = skipString(t, i); continue; }
        if (c === '(') { var d = 1; for (i++; i < t.length && d > 0; i++) { var cc = t.charAt(i); if (cc === '"' || cc === "'") i = skipString(t, i) - 1; else if (cc === '(') d++; else if (cc === ')') d--; } continue; }
        if (c === ';') return { prelude: t.slice(start, i).trim(), body: null, end: i + 1 };
        if (c === '{') { var close = matching(t, i); return { prelude: t.slice(start, i).trim(), body: t.slice(i + 1, close), end: close + 1 }; }
        if (c === '}') return { prelude: t.slice(start, i).trim(), body: null, end: i + 1 };
        i++;
      }
      return { prelude: t.slice(start).trim(), body: null, end: t.length };
    };
    var parseDeclarations = function (body) {
      var decls = [], i = 0;
      while (i < body.length) {
        var st = nextStatement(body, i);
        i = st.end;
        if (st.body !== null || st.prelude === '') continue;
        var colon = st.prelude.indexOf(':');
        if (colon <= 0) continue;
        var name = st.prelude.slice(0, colon).trim(), value = st.prelude.slice(colon + 1).trim(), important = false;
        var m = /\s*!\s*important\s*$/i.exec(value);
        if (m) { important = true; value = value.slice(0, m.index).trim(); }
        if (name.slice(0, 2) !== '--') name = name.toLowerCase();
        if (value !== '') decls.push({ name: name, value: value, important: important });
      }
      return decls;
    };
    // A rule's contents: nested rules and (for style rules) declarations, in the order written.
    var parseList = function (text, parentSheet, parentRule, inStyle) {
      var rules = [], i = 0;
      while (i < text.length) {
        while (i < text.length && /\s/.test(text.charAt(i))) i++;
        if (i >= text.length) break;
        var st = nextStatement(text, i), before = i;
        i = st.end;
        var p = st.prelude;
        if (p === '' && st.body === null) continue;
        if (p.charAt(0) === '@') {
          var m = /^@([\w-]+)\s*([\s\S]*)$/.exec(p);
          if (!m) continue;
          var name = m[1].toLowerCase().replace(/^-(webkit|moz|ms|o)-/, ''), rest = m[2].trim();
          if (st.body === null) {
            if (name === 'import') rules.push(makeRule('import', parentSheet, parentRule, { text: rest }));
            else if (name === 'namespace') rules.push(makeRule('namespace', parentSheet, parentRule, { text: rest }));
            else if (name === 'charset') continue;
            else if (name === 'layer') rules.push(makeRule('layer-statement', parentSheet, parentRule, { text: rest }));
            else rules.push(makeRule('unknown', parentSheet, parentRule, { text: p }));
          } else {
            rules.push(makeRule(name, parentSheet, parentRule, { prelude: rest, body: st.body, name: name }));
          }
        } else if (st.body !== null) {
          if (p === '') continue;   // (a block without a selector is not a rule)
          rules.push(makeRule('style', parentSheet, parentRule, { prelude: p, body: st.body }));
        }
      }
      return rules;
    };

    // --- the rules
    function makeRule(kind, sheet, parent, src) {
      var C, rule;
      switch (kind) {
        case 'style': C = CSSStyleRule; break;
        case 'import': C = CSSImportRule; break;
        case 'media': C = CSSMediaRule; break;
        case 'supports': C = CSSSupportsRule; break;
        case 'container': C = CSSContainerRule; break;
        case 'layer': C = CSSLayerBlockRule; break;
        case 'layer-statement': C = CSSLayerStatementRule; break;
        case 'font-face': C = CSSFontFaceRule; break;
        case 'page': C = CSSPageRule; break;
        case 'keyframes': C = CSSKeyframesRule; break;
        case 'namespace': C = CSSNamespaceRule; break;
        case 'property': C = CSSPropertyRule; break;
        case 'counter-style': C = CSSCounterStyleRule; break;
        case 'scope': C = CSSScopeRule; break;
        case 'starting-style': C = CSSStartingStyleRule; break;
        case 'keyframe': C = CSSKeyframeRule; break;
        default: C = CSSRule;
      }
      rule = Object.create(C.prototype);
      var st = { kind: kind, sheet: sheet, parent: parent, selector: '', prelude: src.prelude || '', text: src.text || '', rules: [], decls: [], name: src.name || kind, style: null };
      hide(rule, '__r', st);
      if (kind === 'style' || kind === 'font-face' || kind === 'page' || kind === 'keyframe' || kind === 'property' || kind === 'counter-style') {
        st.decls = parseDeclarations(src.body);
        if (kind === 'style') {
          st.selector = src.prelude;
          st.rules = parseList(stripNested(src.body), sheet, rule, true);
        } else if (kind === 'page') st.selector = src.prelude;
        else if (kind === 'keyframe') st.selector = src.prelude;
        else if (kind === 'property') st.selector = src.prelude;
        else if (kind === 'counter-style') st.selector = src.prelude;
        st.style = makeDeclaration(st.decls, function () { commit(st.sheet); });
      } else if (kind === 'keyframes') {
        st.rules = parseList(src.body, sheet, rule, false).map(function (r) { return r; });
        st.rules = parseKeyframes(src.body, sheet, rule);
      } else if (src.body !== undefined && src.body !== null) {
        st.rules = parseList(src.body, sheet, rule, false);
      }
      return rule;
    }
    var parseKeyframes = function (body, sheet, parent) {
      var out = [], i = 0;
      while (i < body.length) {
        while (i < body.length && /\s/.test(body.charAt(i))) i++;
        if (i >= body.length) break;
        var st = nextStatement(body, i);
        i = st.end;
        if (st.body !== null) out.push(makeRule('keyframe', sheet, parent, { prelude: st.prelude, body: st.body }));
      }
      return out;
    };
    // the body of a style rule without its declarations: what is left are nested rules
    var stripNested = function (body) {
      var out = '', i = 0;
      while (i < body.length) {
        var st = nextStatement(body, i);
        if (st.body !== null) out += body.slice(i, st.end);
        i = st.end;
      }
      return out;
    };
    var declText = function (decls) { return decls.map(function (d) { return d.name + ': ' + d.value + (d.important ? ' !important' : '') + ';'; }).join(' '); };
    var ruleText = function (rule) {
      var r = rule.__r;
      switch (r.kind) {
        case 'style': {
          var inner = declText(r.decls), nested = r.rules.map(ruleText).join(' ');
          return r.selector + ' { ' + (inner ? inner + (nested ? ' ' : '') : '') + nested + (inner || nested ? ' ' : '') + '}';
        }
        case 'font-face': case 'page': case 'keyframe': case 'property': case 'counter-style':
          return '@' + (r.kind === 'keyframe' ? '' : r.kind + ' ') + (r.kind === 'keyframe' ? '' : r.selector ? r.selector + ' ' : '') + (r.kind === 'keyframe' ? r.selector + ' ' : '') + '{ ' + declText(r.decls) + (r.decls.length ? ' ' : '') + '}';
        case 'import': return '@import ' + r.text + ';';
        case 'namespace': return '@namespace ' + r.text + ';';
        case 'layer-statement': return '@layer ' + r.text + ';';
        case 'unknown': return r.text;
        case 'keyframes': return '@keyframes ' + r.prelude + ' { ' + r.rules.map(ruleText).join(' ') + (r.rules.length ? ' ' : '') + '}';
        default: return '@' + r.name + (r.prelude ? ' ' + r.prelude : '') + ' { ' + r.rules.map(ruleText).join(' ') + (r.rules.length ? ' ' : '') + '}';
      }
    };
    var sheetText = function (sheet) { return sheet.__s.rules.map(ruleText).join('\n'); };
    var commit = function (sheet) {
      var s = sheet && sheet.__s;
      if (!s) return;
      if (s.owner && s.owner.isConnected !== false) {
        s.writing = true;
        s.owner.textContent = sheetText(sheet);
        s.writing = false;
      }
      if (s.constructed) for (var i = 0; i < s.users.length; i++) adoptedSync(s.users[i]);
    };
    defineGetter(CSSRule.prototype, 'type', function () { var k = this.__r.kind; return kinds[k] === undefined ? 0 : kinds[k]; });
    defineGetter(CSSRule.prototype, 'cssText', function () { return ruleText(this); }, function () { });
    defineGetter(CSSRule.prototype, 'parentRule', function () { return this.__r.parent || null; });
    defineGetter(CSSRule.prototype, 'parentStyleSheet', function () { return this.__r.sheet || null; });
    defineGetter(CSSStyleRule.prototype, 'selectorText', function () { return this.__r.selector; }, function (v) { this.__r.selector = String(v).trim(); commit(this.__r.sheet); });
    defineGetter(CSSStyleRule.prototype, 'style', function () { return this.__r.style; });
    defineGetter(CSSStyleRule.prototype, 'styleMap', function () { return this.__r.style; });
    [CSSFontFaceRule, CSSKeyframeRule, CSSPageRule, CSSPropertyRule, CSSCounterStyleRule].forEach(function (C) { defineGetter(C.prototype, 'style', function () { return this.__r.style; }); });
    defineGetter(CSSPageRule.prototype, 'selectorText', function () { return this.__r.selector; }, function (v) { this.__r.selector = String(v); commit(this.__r.sheet); });
    defineGetter(CSSKeyframeRule.prototype, 'keyText', function () { return this.__r.selector; }, function (v) { this.__r.selector = String(v); commit(this.__r.sheet); });
    defineGetter(CSSKeyframesRule.prototype, 'name', function () { return this.__r.prelude; }, function (v) { this.__r.prelude = String(v); commit(this.__r.sheet); });
    defineGetter(CSSPropertyRule.prototype, 'name', function () { return this.__r.selector; });
    ['syntax', 'inherits', 'initialValue'].forEach(function (n) {
      defineGetter(CSSPropertyRule.prototype, n, function () {
        var want = n === 'initialValue' ? 'initial-value' : n, d = this.__r.decls.filter(function (x) { return x.name === want; })[0];
        if (!d) return n === 'inherits' ? false : null;
        return n === 'inherits' ? d.value === 'true' : d.value.replace(/^["']|["']$/g, '');
      });
    });
    defineGetter(CSSCounterStyleRule.prototype, 'name', function () { return this.__r.selector; });
    defineGetter(CSSNamespaceRule.prototype, 'namespaceURI', function () { var m = /url\(\s*["']?([^"')]*)["']?\s*\)|["']([^"']*)["']/.exec(this.__r.text); return m ? m[1] || m[2] : ''; });
    defineGetter(CSSNamespaceRule.prototype, 'prefix', function () { var m = /^([\w-]+)\s+/.exec(this.__r.text); return m ? m[1] : ''; });
    defineGetter(CSSImportRule.prototype, 'href', function () { var m = /^(?:url\(\s*["']?([^"')]*)["']?\s*\)|["']([^"']*)["'])/.exec(this.__r.text); return m ? m[1] || m[2] : ''; });
    defineGetter(CSSImportRule.prototype, 'media', function () { var m = /^(?:url\([^)]*\)|"[^"]*"|'[^']*')\s*([\s\S]*)$/.exec(this.__r.text); return newMediaList(m ? m[1].replace(/\b(layer|supports)\b[\s\S]*$/, '').trim() : ''); });
    defineGetter(CSSImportRule.prototype, 'styleSheet', function () { return null; });
    defineGetter(CSSImportRule.prototype, 'layerName', function () { var m = /\blayer(?:\(([^)]*)\))?/.exec(this.__r.text); return m ? m[1] || '' : null; });
    defineGetter(CSSLayerStatementRule.prototype, 'nameList', function () { return this.__r.text.split(',').map(function (x) { return x.trim(); }); });
    defineGetter(CSSLayerBlockRule.prototype, 'name', function () { return this.__r.prelude; });
    defineGetter(CSSMediaRule.prototype, 'media', function () { return newMediaList(this.__r.prelude, this); });
    defineGetter(CSSMediaRule.prototype, 'conditionText', function () { return this.__r.prelude; });
    defineGetter(CSSSupportsRule.prototype, 'conditionText', function () { return this.__r.prelude; });
    defineGetter(CSSContainerRule.prototype, 'conditionText', function () { return this.__r.prelude.replace(/^[\w-]+\s*(?=\()/, ''); });
    defineGetter(CSSContainerRule.prototype, 'containerName', function () { var m = /^([\w-]+)\s*\(/.exec(this.__r.prelude); return m ? m[1] : ''; });
    // rule lists and the insertRule / deleteRule of everything that has rules
    var listOf = function (owner) {
      var r = owner.__r || owner.__s, list = Object.create(CSSRuleList.prototype);
      hide(list, '__o', owner);
      var items = r.rules;
      defineGetter(list, 'length', function () { return items.length; });
      list.item = function (i) { return items[i >>> 0] || null; };
      list[Symbol.iterator] = function () { return items.slice(); };
      for (var i = 0; i < items.length; i++) list[i] = items[i];
      return list;
    };
    var cssRules = function () { return listOf(this); };
    defineGetter(CSSStyleSheet.prototype, 'cssRules', cssRules);
    defineGetter(CSSStyleSheet.prototype, 'rules', cssRules);
    [CSSGroupingRule, CSSKeyframesRule, CSSStyleRule].forEach(function (C) { defineGetter(C.prototype, 'cssRules', cssRules); });
    defineGetter(CSSKeyframesRule.prototype, 'length', function () { return this.__r.rules.length; });
    var insert = function (owner, text, index, what, parentRule) {
      var st = owner.__r || owner.__s, sheet = owner.__s ? owner : owner.__r.sheet;
      text = String(text);
      if (index === undefined) index = 0;
      index = index >>> 0;
      if (index > st.rules.length) throw new DOMException("Failed to execute '" + what + "' on '" + (owner.__s ? 'CSSStyleSheet' : 'CSSRule') + "': The index provided (" + index + ") is larger than the maximum index (" + st.rules.length + ").", 'IndexSizeError');
      var parsed = parseList(stripComments(text), sheet, parentRule || null, false);
      if (parsed.length !== 1) throw new DOMException("Failed to execute '" + what + "' on '" + (owner.__s ? 'CSSStyleSheet' : 'CSSRule') + "': Failed to parse the rule '" + text + "'.", 'SyntaxError');
      var rule = parsed[0];
      if (rule.__r.kind === 'import' && !(owner.__s)) throw new DOMException("Failed to execute '" + what + "' on 'CSSRule': Failed to parse the rule '" + text + "'.", 'HierarchyRequestError');
      rule.__r.parent = owner.__s ? null : owner;
      st.rules.splice(index, 0, rule);
      commit(sheet);
      return index;
    };
    var remove = function (owner, index, what) {
      var st = owner.__r || owner.__s, sheet = owner.__s ? owner : owner.__r.sheet;
      index = index >>> 0;
      if (index >= st.rules.length) throw new DOMException("Failed to execute '" + what + "' on '" + (owner.__s ? 'CSSStyleSheet' : 'CSSRule') + "': The index provided (" + index + ") is larger than the maximum index (" + (st.rules.length - 1) + ").", 'IndexSizeError');
      st.rules.splice(index, 1);
      commit(sheet);
    };
    method(CSSStyleSheet.prototype, 'insertRule', function (text, index) {
      if (arguments.length < 1) throw typeErr("execute 'insertRule' on 'CSSStyleSheet'", '1 argument required, but only 0 present.');
      if (this.__s.disabledByOwner) throw new DOMException("Failed to execute 'insertRule' on 'CSSStyleSheet': Cannot access StyleSheet to insertRule", 'SecurityError');
      return insert(this, text, index, 'insertRule');
    });
    method(CSSStyleSheet.prototype, 'deleteRule', function (index) { if (arguments.length < 1) throw typeErr("execute 'deleteRule' on 'CSSStyleSheet'", '1 argument required, but only 0 present.'); remove(this, index, 'deleteRule'); });
    method(CSSStyleSheet.prototype, 'addRule', function (selector, style, index) { var s = this.__s; insert(this, selector + ' { ' + (style || '') + ' }', index === undefined ? s.rules.length : index, 'addRule'); return -1; });
    method(CSSStyleSheet.prototype, 'removeRule', function (index) { remove(this, index === undefined ? 0 : index, 'removeRule'); });
    [CSSGroupingRule, CSSStyleRule].forEach(function (C) {
      method(C.prototype, 'insertRule', function (text, index) { return insert(this, text, index, 'insertRule', this); });
      method(C.prototype, 'deleteRule', function (index) { remove(this, index, 'deleteRule'); });
    });
    method(CSSKeyframesRule.prototype, 'appendRule', function (text) {
      var parsed = parseKeyframes(stripComments(String(text)) + '', this.__r.sheet, this);
      if (parsed.length) { this.__r.rules.push(parsed[0]); commit(this.__r.sheet); }
    });
    method(CSSKeyframesRule.prototype, 'deleteRule', function (sel) { var r = this.__r.rules; for (var i = 0; i < r.length; i++) if (r[i].__r.selector === String(sel).trim()) { r.splice(i, 1); commit(this.__r.sheet); return; } });
    method(CSSKeyframesRule.prototype, 'findRule', function (sel) { var r = this.__r.rules; for (var i = 0; i < r.length; i++) if (r[i].__r.selector === String(sel).trim()) return r[i]; return null; });
    // --- media lists
    function newMediaList(text, rule) {
      var list = Object.create(MediaList.prototype);
      hide(list, '__m', { text: text, rule: rule || null });
      return list;
    }
    var mediaItems = function (l) { return l.__m.text.trim() === '' ? [] : l.__m.text.split(',').map(function (x) { return x.trim(); }).filter(Boolean); };
    var setMedia = function (l, text) { l.__m.text = text; if (l.__m.rule) { l.__m.rule.__r.prelude = text; commit(l.__m.rule.__r.sheet); } else if (l.__m.sheet) { commit(l.__m.sheet); } };
    defineGetter(MediaList.prototype, 'mediaText', function () { return mediaItems(this).join(', '); }, function (v) { setMedia(this, String(v)); });
    defineGetter(MediaList.prototype, 'length', function () { return mediaItems(this).length; });
    method(MediaList.prototype, 'item', function (i) { var a = mediaItems(this); return i < a.length ? a[i] : null; });
    method(MediaList.prototype, 'appendMedium', function (m) { var a = mediaItems(this); if (a.indexOf(String(m)) < 0) a.push(String(m)); setMedia(this, a.join(', ')); });
    method(MediaList.prototype, 'deleteMedium', function (m) { var a = mediaItems(this).filter(function (x) { return x !== String(m); }); setMedia(this, a.join(', ')); });
    method(MediaList.prototype, 'toString', function () { return this.mediaText; });

    // --- declarations
    var props = ('align-content align-items align-self all animation animation-delay animation-direction animation-duration animation-fill-mode animation-iteration-count animation-name animation-play-state animation-timing-function appearance aspect-ratio backdrop-filter backface-visibility background background-attachment background-blend-mode background-clip background-color background-image background-origin background-position background-position-x background-position-y background-repeat background-size block-size border border-block border-block-color border-block-end border-block-start border-block-style border-block-width border-bottom border-bottom-color border-bottom-left-radius border-bottom-right-radius border-bottom-style border-bottom-width border-collapse border-color border-image border-image-outset border-image-repeat border-image-slice border-image-source border-image-width border-inline border-inline-color border-inline-end border-inline-start border-inline-style border-inline-width border-left border-left-color border-left-style border-left-width border-radius border-right border-right-color border-right-style border-right-width border-spacing border-style border-top border-top-color border-top-left-radius border-top-right-radius border-top-style border-top-width border-width bottom box-shadow box-sizing break-after break-before break-inside caption-side caret-color clear clip clip-path color color-scheme column-count column-fill column-gap column-rule column-rule-color column-rule-style column-rule-width column-span column-width columns contain container container-name container-type content counter-increment counter-reset counter-set cursor direction display empty-cells fill filter flex flex-basis flex-direction flex-flow flex-grow flex-shrink flex-wrap float font font-family font-feature-settings font-kerning font-size font-size-adjust font-stretch font-style font-variant font-variant-caps font-variant-ligatures font-variant-numeric font-variation-settings font-weight gap grid grid-area grid-auto-columns grid-auto-flow grid-auto-rows grid-column grid-column-end grid-column-gap grid-column-start grid-gap grid-row grid-row-end grid-row-gap grid-row-start grid-template grid-template-areas grid-template-columns grid-template-rows height hyphens image-rendering inline-size inset inset-block inset-block-end inset-block-start inset-inline inset-inline-end inset-inline-start isolation justify-content justify-items justify-self left letter-spacing line-break line-height list-style list-style-image list-style-position list-style-type margin margin-block margin-block-end margin-block-start margin-bottom margin-inline margin-inline-end margin-inline-start margin-left margin-right margin-top mask mask-image max-block-size max-height max-inline-size max-width min-block-size min-height min-inline-size min-width mix-blend-mode object-fit object-position offset opacity order orphans outline outline-color outline-offset outline-style outline-width overflow overflow-anchor overflow-wrap overflow-x overflow-y overscroll-behavior padding padding-block padding-block-end padding-block-start padding-bottom padding-inline padding-inline-end padding-inline-start padding-left padding-right padding-top page-break-after page-break-before page-break-inside perspective perspective-origin place-content place-items place-self pointer-events position quotes resize right rotate row-gap scale scroll-behavior scroll-margin scroll-padding scroll-snap-align scroll-snap-type scrollbar-color scrollbar-gutter scrollbar-width shape-outside tab-size table-layout text-align text-align-last text-decoration text-decoration-color text-decoration-line text-decoration-style text-decoration-thickness text-indent text-justify text-overflow text-shadow text-transform text-underline-offset text-underline-position top touch-action transform transform-box transform-origin transform-style transition transition-delay transition-duration transition-property transition-timing-function translate unicode-bidi user-select vertical-align visibility white-space widows width will-change word-break word-spacing word-wrap writing-mode z-index zoom -webkit-appearance -webkit-line-clamp -webkit-text-fill-color -webkit-text-stroke -webkit-box-orient -webkit-tap-highlight-color -webkit-font-smoothing -webkit-mask-image -webkit-backdrop-filter').split(' ');
    var camel = function (p) { return p.replace(/^-webkit-/, 'webkit-').replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); };
    var shorthands = { margin: ['margin-top', 'margin-right', 'margin-bottom', 'margin-left'], padding: ['padding-top', 'padding-right', 'padding-bottom', 'padding-left'] };
    function makeDeclaration(decls, onChange) {
      var d = Object.create(CSSStyleDeclaration2.prototype), st = { decls: decls, change: onChange, parent: null };
      hide(d, '__d', st);
      return d;
    }
    var find = function (st, name) { for (var i = 0; i < st.decls.length; i++) if (st.decls[i].name === name) return i; return -1; };
    var norm = function (n) { n = String(n); return n.slice(0, 2) === '--' ? n : n.toLowerCase(); };
    var changed = function (d) { if (d.__d.change) d.__d.change(); };
    defineGetter(CSSStyleDeclaration2.prototype, 'cssText', function () { return declText(this.__d.decls); }, function (v) { this.__d.decls = parseDeclarations(stripComments(String(v))); changed(this); });
    defineGetter(CSSStyleDeclaration2.prototype, 'length', function () { return this.__d.decls.length; });
    defineGetter(CSSStyleDeclaration2.prototype, 'parentRule', function () { return this.__d.parent || null; });
    method(CSSStyleDeclaration2.prototype, 'item', function (i) { var d = this.__d.decls[i >>> 0]; return d ? d.name : ''; });
    method(CSSStyleDeclaration2.prototype, 'getPropertyValue', function (n) { var i = find(this.__d, norm(n)); return i < 0 ? '' : this.__d.decls[i].value; });
    method(CSSStyleDeclaration2.prototype, 'getPropertyPriority', function (n) { var i = find(this.__d, norm(n)); return i >= 0 && this.__d.decls[i].important ? 'important' : ''; });
    method(CSSStyleDeclaration2.prototype, 'setProperty', function (n, v, priority) {
      if (arguments.length < 2) throw typeErr("execute 'setProperty' on 'CSSStyleDeclaration'", '2 arguments required, but only ' + arguments.length + ' present.');
      n = norm(n); v = v === null ? '' : String(v);
      if (v === '') { this.removeProperty(n); return; }
      var important = String(priority || '').toLowerCase() === 'important', i = find(this.__d, n);
      if (i >= 0) { this.__d.decls[i].value = v; this.__d.decls[i].important = important; } else this.__d.decls.push({ name: n, value: v, important: important });
      changed(this);
    });
    method(CSSStyleDeclaration2.prototype, 'removeProperty', function (n) {
      n = norm(n); var i = find(this.__d, n), old = '';
      if (i >= 0) { old = this.__d.decls[i].value; this.__d.decls.splice(i, 1); changed(this); }
      return old;
    });
    defineGetter(CSSStyleDeclaration2.prototype, 'cssFloat', function () { return this.getPropertyValue('float'); }, function (v) { this.setProperty('float', v); });
    props.forEach(function (p) {
      var accessorGet = function () { return this.getPropertyValue(p); }, accessorSet = function (v) { this.setProperty(p, v === null ? '' : v); };
      defineGetter(CSSStyleDeclaration2.prototype, camel(p), accessorGet, accessorSet);
      if (camel(p) !== p) defineGetter(CSSStyleDeclaration2.prototype, p, accessorGet, accessorSet);
    });
    method(CSSStyleDeclaration2.prototype, Symbol.iterator, function () { return this.__d.decls.map(function (d) { return d.name; }); });

    // --- sheets
    function initSheet(sheet, owner, options) {
      var s = { rules: [], owner: owner, constructed: false, users: [], disabled: false, media: newMediaList(''), title: null, href: null, parent: null, ownerRule: null, writing: false };
      hide(sheet, '__s', s);
      if (!owner) {
        s.constructed = true;
        if (options.media !== undefined) s.media = newMediaList(typeof options.media === 'string' ? options.media : options.media.mediaText);
        s.media.__m.sheet = sheet;
        s.disabled = !!options.disabled;
      }
      return s;
    }
    var readInto = function (sheet, text) {
      var s = sheet.__s;
      s.rules = parseList(stripComments(String(text)), sheet, null, false);
      s.rules = s.rules.filter(function (r) { return r; });
    };
    defineGetter(StyleSheet.prototype, 'type', function () { return 'text/css'; });
    defineGetter(StyleSheet.prototype, 'href', function () { return this.__s.href; });
    defineGetter(StyleSheet.prototype, 'ownerNode', function () { return this.__s.owner || null; });
    defineGetter(StyleSheet.prototype, 'parentStyleSheet', function () { return null; });
    defineGetter(StyleSheet.prototype, 'title', function () { return this.__s.owner ? this.__s.owner.getAttribute('title') : null; });
    defineGetter(StyleSheet.prototype, 'media', function () { return this.__s.media; });
    defineGetter(StyleSheet.prototype, 'disabled', function () { return this.__s.disabled; }, function (v) {
      var s = this.__s; v = !!v;
      if (s.disabled === v) return;
      s.disabled = v;
      if (s.owner) { if (v) s.owner.setAttribute('media', 'not all'); else s.owner.removeAttribute('media'); }
    });
    defineGetter(CSSStyleSheet.prototype, 'ownerRule', function () { return this.__s.ownerRule; });
    method(CSSStyleSheet.prototype, 'replaceSync', function (text) {
      if (!this.__s.constructed) throw new DOMException("Failed to execute 'replaceSync' on 'CSSStyleSheet': Can't call replaceSync on non-constructed CSSStyleSheets.", 'NotAllowedError');
      readInto(this, text); commit(this);
    });
    method(CSSStyleSheet.prototype, 'replace', function (text) {
      var self = this;
      if (!this.__s.constructed) return Promise.reject(new DOMException("Failed to execute 'replace' on 'CSSStyleSheet': Can't call replace on non-constructed CSSStyleSheets.", 'NotAllowedError'));
      return new Promise(function (resolve) { later(function () { readInto(self, text); commit(self); resolve(self); }); });
    });
    // The sheet of a <style>: made when asked for, re-read when the element's text changed.
    var sheets = [];
    hide(g, '__sheet_of', function (el) {
      if (!el || (el.localName !== 'style' && el.localName !== 'link') || !el.isConnected) return null;
      var sheet = el.__sheet;
      if (el.localName === 'link') {
        var rel = (el.getAttribute('rel') || '').toLowerCase().split(/\s+/);
        if (rel.indexOf('stylesheet') < 0) return null;
        if (!sheet) {
          sheet = Object.create(CSSStyleSheet.prototype);
          var ls = initSheet(sheet, el, {});
          ls.href = el.href || null;   // (what a linked file says is read by the browser itself, not by script)
          Object.defineProperty(el, '__sheet', { value: sheet, configurable: true });
          sheets.push(sheet);
        }
        return sheet;
      }
      var type = el.getAttribute('type');
      if (type !== null && type !== '' && type.toLowerCase() !== 'text/css') return null;
      var text = el.textContent;
      if (!sheet) {
        sheet = Object.create(CSSStyleSheet.prototype);
        initSheet(sheet, el, {});
        Object.defineProperty(el, '__sheet', { value: sheet, configurable: true });
        sheets.push(sheet);
        sheet.__s.read = null;
      }
      if (sheet.__s.read !== text && !sheet.__s.writing) { readInto(sheet, text); sheet.__s.read = text; }
      return sheet;
    });
    // document.styleSheets: the sheets of the <style> and <link rel=stylesheet> elements, in tree order
    var list = function () {
      var out = [], els = doc.querySelectorAll('style, link[rel~="stylesheet" i]');
      for (var i = 0; i < els.length; i++) { var s = g.__sheet_of(els[i]); if (s) out.push(s); }
      var sl = Object.create(StyleSheetList.prototype);
      defineGetter(sl, 'length', function () { return out.length; });
      sl.item = function (i) { return out[i >>> 0] || null; };
      for (var k = 0; k < out.length; k++) sl[k] = out[k];
      sl[Symbol.iterator] = function () { return out.slice(); };
      return sl;
    };
    Object.defineProperty(DP, 'styleSheets', { get: list, configurable: true, enumerable: true });
    // adoptedStyleSheets: constructed sheets that style the document (or a shadow root); they are carried by a hidden <style> each
    var adopted = [];
    var adoptedSync = function (holder) {
      var all = holder.sheets, text = all.map(function (sh) { return sheetText(sh); }).join('\n');
      if (!holder.node) {
        holder.node = doc.createElement('style');
        holder.node.setAttribute('data-adopted-style-sheets', '');
        (holder.root === doc ? (doc.head || doc.documentElement) : holder.root).appendChild(holder.node);
      }
      holder.node.textContent = text;
    };
    var holderOf = function (root) {
      for (var i = 0; i < adopted.length; i++) if (adopted[i].root === root) return adopted[i];
      var h = { root: root, sheets: [], node: null };
      adopted.push(h);
      return h;
    };
    var setAdopted = function (value) {
      var h = holderOf(this === DP || this === doc ? doc : this);
      if (!value || typeof value.length !== 'number') throw typeErr("set the 'adoptedStyleSheets' property on 'Document'", "The provided value is not a sequence.");
      var next = Array.prototype.slice.call(value);
      next.forEach(function (sh) {
        if (!(sh instanceof CSSStyleSheet) || !sh.__s.constructed) throw new TypeError("Failed to set the 'adoptedStyleSheets' property on 'Document': Failed to convert value to 'CSSStyleSheet'.");
      });
      h.sheets.forEach(function (sh) { var at = sh.__s.users.indexOf(h); if (at >= 0) sh.__s.users.splice(at, 1); });
      h.sheets = next;
      next.forEach(function (sh) { if (sh.__s.users.indexOf(h) < 0) sh.__s.users.push(h); });
      adoptedSync(h);
    };
    var getAdopted = function () { return holderOf(this === DP || this === doc ? doc : this).sheets.slice(); };
    Object.defineProperty(DP, 'adoptedStyleSheets', { get: getAdopted, set: setAdopted, configurable: true, enumerable: true });
    if (g.ShadowRoot) Object.defineProperty(g.ShadowRoot.prototype, 'adoptedStyleSheets', { get: getAdopted, set: setAdopted, configurable: true, enumerable: true });
    if (g.ShadowRoot && !('styleSheets' in g.ShadowRoot.prototype)) Object.defineProperty(g.ShadowRoot.prototype, 'styleSheets', { get: function () { var sl = Object.create(StyleSheetList.prototype); defineGetter(sl, 'length', function () { return 0; }); return sl; }, configurable: true });
    defineGetter(StyleSheetList.prototype, 'length', function () { return 0; });
    method(StyleSheetList.prototype, 'item', function () { return null; });
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- XPath 1.0
  (function () {
    var XPathResult = klass('XPathResult', null), XPathExpression = klass('XPathExpression', null), XPathEvaluator = klass('XPathEvaluator', null, function () { }), XPathNSResolver = klass('XPathNSResolver', null);
    var T = { ANY: 0, NUMBER: 1, STRING: 2, BOOLEAN: 3, UNORDERED_NODE_ITERATOR: 4, ORDERED_NODE_ITERATOR: 5, UNORDERED_NODE_SNAPSHOT: 6, ORDERED_NODE_SNAPSHOT: 7, ANY_UNORDERED_NODE: 8, FIRST_ORDERED_NODE: 9 };
    var names = { ANY_TYPE: 0, NUMBER_TYPE: 1, STRING_TYPE: 2, BOOLEAN_TYPE: 3, UNORDERED_NODE_ITERATOR_TYPE: 4, ORDERED_NODE_ITERATOR_TYPE: 5, UNORDERED_NODE_SNAPSHOT_TYPE: 6, ORDERED_NODE_SNAPSHOT_TYPE: 7, ANY_UNORDERED_NODE_TYPE: 8, FIRST_ORDERED_NODE_TYPE: 9 };
    Object.keys(names).forEach(function (k) { XPathResult[k] = names[k]; XPathResult.prototype[k] = names[k]; });
    var xerr = function (message, name) { return new DOMException(message, name || 'SyntaxError'); };

    // --- lexer
    var lex = function (src) {
      var toks = [], i = 0, n = src.length;
      var isNameStart = function (c) { return /[A-Za-z_\u0080-￿]/.test(c); }, isNameChar = function (c) { return /[A-Za-z0-9_.\-\u0080-￿]/.test(c); };
      while (i < n) {
        var c = src.charAt(i);
        if (/\s/.test(c)) { i++; continue; }
        var prev = toks.length ? toks[toks.length - 1] : null;
        // after one of these, * and a name are operators
        var operandBefore = prev && !(prev.t === 'op' || prev.t === '(' || prev.t === '[' || prev.t === ',' || prev.t === '::' || prev.t === '@' || prev.t === '/' || prev.t === '//' || prev.t === '|');
        if (c === '"' || c === "'") { var e = src.indexOf(c, i + 1); if (e < 0) throw xerr('Unterminated string literal in XPath expression.'); toks.push({ t: 'str', v: src.slice(i + 1, e) }); i = e + 1; continue; }
        if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src.charAt(i + 1)))) { var m = /^[0-9]*\.?[0-9]*/.exec(src.slice(i))[0]; toks.push({ t: 'num', v: parseFloat(m) }); i += m.length; continue; }
        if (c === '/' && src.charAt(i + 1) === '/') { toks.push({ t: '//' }); i += 2; continue; }
        if (c === '.' && src.charAt(i + 1) === '.') { toks.push({ t: '..' }); i += 2; continue; }
        if (c === ':' && src.charAt(i + 1) === ':') { toks.push({ t: '::' }); i += 2; continue; }
        if (c === '!' && src.charAt(i + 1) === '=') { toks.push({ t: 'op', v: '!=' }); i += 2; continue; }
        if (c === '<' || c === '>') { if (src.charAt(i + 1) === '=') { toks.push({ t: 'op', v: c + '=' }); i += 2; } else { toks.push({ t: 'op', v: c }); i++; } continue; }
        if (c === '=' || c === '+' || c === '-' && false) { toks.push({ t: 'op', v: c }); i++; continue; }
        if ('()[],@/|.'.indexOf(c) >= 0) { toks.push({ t: c }); i++; continue; }
        if (c === '$') { throw xerr('XPath variables are not supported.'); }
        if (c === '*') { toks.push(operandBefore ? { t: 'op', v: '*' } : { t: 'name', v: '*' }); i++; continue; }
        if (isNameStart(c) || (c === '-' && false)) {
          var j = i;
          while (j < n && isNameChar(src.charAt(j))) j++;
          // a prefix: name:name or name:*
          if (src.charAt(j) === ':' && src.charAt(j + 1) !== ':') {
            if (src.charAt(j + 1) === '*') j += 2;
            else if (isNameStart(src.charAt(j + 1))) { j++; while (j < n && isNameChar(src.charAt(j))) j++; }
          }
          var word = src.slice(i, j);
          if (operandBefore && (word === 'and' || word === 'or' || word === 'mod' || word === 'div')) toks.push({ t: 'op', v: word });
          else toks.push({ t: 'name', v: word });
          i = j;
          continue;
        }
        if (c === '-') { toks.push({ t: 'op', v: '-' }); i++; continue; }
        throw xerr("The string '" + src + "' is not a valid XPath expression.");
      }
      toks.push({ t: 'end' });
      return toks;
    };
    // a '-' directly after a name character is part of the name (handled above); a '-' elsewhere is an operator

    // --- parser
    var AXES = { child: 1, descendant: 1, parent: 1, ancestor: 1, 'following-sibling': 1, 'preceding-sibling': 1, following: 1, preceding: 1, attribute: 1, namespace: 1, self: 1, 'descendant-or-self': 1, 'ancestor-or-self': 1 };
    var parse = function (src) {
      var toks = lex(src), p = 0;
      var peek = function () { return toks[p]; }, next = function () { return toks[p++]; };
      var bad = function () { return xerr("The string '" + src + "' is not a valid XPath expression."); };
      var isOp = function (v) { return peek().t === 'op' && peek().v === v; };
      var expect = function (t) { if (peek().t !== t) throw bad(); return next(); };
      var parseOr = function () { var l = parseAnd(); while (isOp('or')) { next(); l = { k: 'or', a: l, b: parseAnd() }; } return l; };
      var parseAnd = function () { var l = parseEq(); while (isOp('and')) { next(); l = { k: 'and', a: l, b: parseEq() }; } return l; };
      var parseEq = function () { var l = parseRel(); while (isOp('=') || isOp('!=')) { var o = next().v; l = { k: 'cmp', o: o, a: l, b: parseRel() }; } return l; };
      var parseRel = function () { var l = parseAdd(); while (isOp('<') || isOp('>') || isOp('<=') || isOp('>=')) { var o = next().v; l = { k: 'cmp', o: o, a: l, b: parseAdd() }; } return l; };
      var parseAdd = function () { var l = parseMul(); while (isOp('+') || isOp('-')) { var o = next().v; l = { k: 'arith', o: o, a: l, b: parseMul() }; } return l; };
      var parseMul = function () { var l = parseUnary(); while (isOp('*') || isOp('div') || isOp('mod')) { var o = next().v; l = { k: 'arith', o: o, a: l, b: parseUnary() }; } return l; };
      var parseUnary = function () { if (isOp('-')) { next(); return { k: 'neg', a: parseUnary() }; } return parseUnion(); };
      var parseUnion = function () { var l = parsePath(); while (peek().t === '|') { next(); l = { k: 'union', a: l, b: parsePath() }; } return l; };
      var startsStep = function () { var t = peek().t; return t === 'name' || t === '@' || t === '.' || t === '..' || t === '*'; };
      var parsePath = function () {
        var t = peek();
        if (t.t === '/' || t.t === '//') {
          next();
          var steps = [];
          if (t.t === '//') steps.push({ axis: 'descendant-or-self', test: { k: 'node' }, preds: [] });
          if (t.t === '/' && !startsStep()) return { k: 'path', root: true, steps: [] };
          parseRelative(steps);
          return { k: 'path', root: true, steps: steps };
        }
        // a filter expression (primary), or a relative location path
        var isFilter = t.t === 'num' || t.t === 'str' || t.t === '(' || (t.t === 'name' && toks[p + 1].t === '(' && ['node', 'text', 'comment', 'processing-instruction'].indexOf(t.v) < 0);
        if (isFilter) {
          var primary = parsePrimary(), preds = [];
          while (peek().t === '[') preds.push(parsePredicate());
          var f = preds.length ? { k: 'filter', a: primary, preds: preds } : primary;
          if (peek().t === '/' || peek().t === '//') {
            var steps2 = [];
            if (next().t === '//') steps2.push({ axis: 'descendant-or-self', test: { k: 'node' }, preds: [] });
            parseRelative(steps2);
            return { k: 'path', from: f, steps: steps2 };
          }
          return f;
        }
        var steps3 = [];
        parseRelative(steps3);
        return { k: 'path', root: false, steps: steps3 };
      };
      var parseRelative = function (steps) {
        steps.push(parseStep());
        while (peek().t === '/' || peek().t === '//') {
          if (next().t === '//') steps.push({ axis: 'descendant-or-self', test: { k: 'node' }, preds: [] });
          steps.push(parseStep());
        }
      };
      var parsePredicate = function () { expect('['); var e = parseOr(); expect(']'); return e; };
      var parseStep = function () {
        var t = peek();
        if (t.t === '.') { next(); return { axis: 'self', test: { k: 'node' }, preds: [] }; }
        if (t.t === '..') { next(); return { axis: 'parent', test: { k: 'node' }, preds: [] }; }
        var axis = 'child';
        if (t.t === '@') { next(); axis = 'attribute'; }
        else if (t.t === 'name' && toks[p + 1].t === '::') { if (!AXES[t.v]) throw bad(); axis = t.v; next(); next(); }
        var nt = next(), test;
        if (nt.t !== 'name') throw bad();
        if (peek().t === '(' && ['node', 'text', 'comment', 'processing-instruction'].indexOf(nt.v) >= 0) {
          next();
          test = { k: nt.v };
          if (nt.v === 'processing-instruction' && peek().t === 'str') test.target = next().v;
          expect(')');
        } else test = { k: 'name', v: nt.v };
        var preds = [];
        while (peek().t === '[') preds.push(parsePredicate());
        return { axis: axis, test: test, preds: preds };
      };
      var parsePrimary = function () {
        var t = next();
        if (t.t === 'num') return { k: 'num', v: t.v };
        if (t.t === 'str') return { k: 'str', v: t.v };
        if (t.t === '(') { var e = parseOr(); expect(')'); return e; }
        if (t.t === 'name' && peek().t === '(') {
          next();
          var args = [];
          if (peek().t !== ')') { args.push(parseOr()); while (peek().t === ',') { next(); args.push(parseOr()); } }
          expect(')');
          return { k: 'call', n: t.v, args: args };
        }
        throw bad();
      };
      var e = parseOr();
      if (peek().t !== 'end') throw bad();
      return e;
    };

    // --- evaluation
    var isAttr = function (n) { return n && n.nodeType === 2; };
    var docOf = function (n) { return n.nodeType === 9 ? n : n.ownerDocument || (n.ownerElement && n.ownerElement.ownerDocument); };
    var order = function (nodes) {   // document order, no duplicates
      var seen = [], out = [];
      for (var i = 0; i < nodes.length; i++) if (seen.indexOf(nodes[i]) < 0) { seen.push(nodes[i]); out.push(nodes[i]); }
      return out.sort(function (a, b) {
        if (a === b) return 0;
        var x = isAttr(a) ? a.ownerElement : a, y = isAttr(b) ? b.ownerElement : b;
        if (x === y) return isAttr(a) && isAttr(b) ? 0 : isAttr(a) ? 1 : -1;
        var pos = x.compareDocumentPosition(y);
        return pos & 4 ? -1 : pos & 2 ? 1 : 0;
      });
    };
    var attrNodes = function (el) {
      if (!el.attributes) return [];
      var out = [];
      for (var i = 0; i < el.attributes.length; i++) {
        var a = el.attributes[i];
        if (a.name === 'xmlns' || a.name.indexOf('xmlns:') === 0) continue;
        var cache = el.__xa || (Object.defineProperty(el, '__xa', { value: {}, configurable: true }), el.__xa);
        var node = cache[a.name];
        if (!node) { node = { nodeType: 2, nodeName: a.name, name: a.name, localName: a.name.replace(/^.*:/, ''), prefix: a.name.indexOf(':') > 0 ? a.name.split(':')[0] : null, namespaceURI: null, ownerElement: el, ownerDocument: docOf(el), parentNode: null, childNodes: [], nodeValue: a.value }; cache[a.name] = node; }
        node.value = node.nodeValue = a.value;
        out.push(node);
      }
      return out;
    };
    var strValue = function (n) {
      if (isAttr(n)) return n.value;
      if (n.nodeType === 3 || n.nodeType === 4 || n.nodeType === 8 || n.nodeType === 7) return n.data;
      var s = '';
      var walk = function (x) { for (var c = x.firstChild; c; c = c.nextSibling) { if (c.nodeType === 3 || c.nodeType === 4) s += c.data; else if (c.nodeType === 1) walk(c); } };
      walk(n);
      return s;
    };
    var toNumber = function (v) {
      if (typeof v === 'number') return v;
      if (typeof v === 'boolean') return v ? 1 : 0;
      var s = toString(v).trim();
      return /^-?(\d+\.?\d*|\.\d+)$/.test(s) ? parseFloat(s) : NaN;
    };
    var numToString = function (n) {
      if (n !== n) return 'NaN';
      if (n === Infinity) return 'Infinity';
      if (n === -Infinity) return '-Infinity';
      if (n === 0) return '0';
      var s = String(n);
      if (s.indexOf('e') >= 0) { s = n.toFixed(20).replace(/0+$/, '').replace(/\.$/, ''); }
      return s;
    };
    var toString = function (v) {
      if (typeof v === 'string') return v;
      if (typeof v === 'number') return numToString(v);
      if (typeof v === 'boolean') return v ? 'true' : 'false';
      return v.length ? strValue(order(v)[0]) : '';
    };
    var toBoolean = function (v) { return typeof v === 'boolean' ? v : typeof v === 'number' ? v !== 0 && v === v : typeof v === 'string' ? v.length > 0 : v.length > 0; };
    var isNodeSet = function (v) { return typeof v === 'object'; };
    var htmlDoc = function (d) { return d && d.contentType === 'text/html'; };
    var matchName = function (node, test, ctx) {
      var v = test.v;
      if (v === '*') return true;
      var local = node.localName === undefined || node.localName === null ? node.nodeName : node.localName;
      var colon = v.indexOf(':');
      if (colon < 0) {
        if (isAttr(node)) return node.name === v;
        if (node.nodeType !== 1) return false;
        // in an HTML document a name without a prefix matches the elements of HTML in any case
        if (htmlDoc(docOf(node)) && (!node.namespaceURI || node.namespaceURI === 'http://www.w3.org/1999/xhtml')) return local.toLowerCase() === v.toLowerCase();
        return local === v && !node.namespaceURI;
      }
      var prefix = v.slice(0, colon), name = v.slice(colon + 1), ns = ctx.resolve(prefix);
      if (ns === null || ns === undefined) throw xerr("The string '" + v + "' contains an unresolved namespace prefix.", 'NamespaceError');
      if (isAttr(node)) return false;
      return (name === '*' || local === name) && node.namespaceURI === ns;
    };
    var testNode = function (node, step, ctx) {
      var t = step.test;
      switch (t.k) {
        case 'node': return true;
        case 'text': return node.nodeType === 3 || node.nodeType === 4;
        case 'comment': return node.nodeType === 8;
        case 'processing-instruction': return node.nodeType === 7 && (t.target === undefined || node.target === t.target);
        default:
          if (step.axis === 'attribute') return isAttr(node) && matchName(node, t, ctx);
          return node.nodeType === 1 && matchName(node, t, ctx);
      }
    };
    var descendants = function (n, out, self) {
      if (self) out.push(n);
      for (var c = n.firstChild; c; c = c.nextSibling) descendants(c, out, true);
    };
    var axisNodes = function (n, axis) {
      var out = [], x;
      switch (axis) {
        case 'child': for (x = n.firstChild; x; x = x.nextSibling) out.push(x); break;
        case 'descendant': for (x = n.firstChild; x; x = x.nextSibling) descendants(x, out, true); break;
        case 'descendant-or-self': descendants(n, out, true); break;
        case 'self': out.push(n); break;
        case 'parent': if (isAttr(n)) out.push(n.ownerElement); else if (n.parentNode) out.push(n.parentNode); break;
        case 'ancestor': case 'ancestor-or-self': {
          if (axis === 'ancestor-or-self') out.push(n);
          x = isAttr(n) ? n.ownerElement : n.parentNode;
          while (x) { out.push(x); x = x.parentNode; }
          break;
        }
        case 'following-sibling': if (!isAttr(n)) for (x = n.nextSibling; x; x = x.nextSibling) out.push(x); break;
        case 'preceding-sibling': if (!isAttr(n)) for (x = n.previousSibling; x; x = x.previousSibling) out.push(x); break;
        case 'following': {
          var start = isAttr(n) ? n.ownerElement : n;
          for (x = start; x; x = x.parentNode) for (var s = x.nextSibling; s; s = s.nextSibling) descendants(s, out, true);
          if (isAttr(n)) for (x = n.ownerElement.firstChild; x; x = x.nextSibling) descendants(x, out, true);
          break;
        }
        case 'preceding': {
          var from = isAttr(n) ? n.ownerElement : n, blocked = [];
          for (x = from.parentNode; x; x = x.parentNode) blocked.push(x);
          var all = []; descendants(docOf(from), all, true);
          for (var i = 0; i < all.length; i++) { if (all[i] === from) break; if (blocked.indexOf(all[i]) < 0) out.push(all[i]); }
          out.reverse();
          break;
        }
        case 'attribute': if (n.nodeType === 1) out = attrNodes(n); break;
        case 'namespace': break;
      }
      return out;
    };
    var reverseAxis = { ancestor: 1, 'ancestor-or-self': 1, 'preceding-sibling': 1, preceding: 1 };
    var evalPreds = function (nodes, preds, ctx) {
      for (var i = 0; i < preds.length; i++) {
        var kept = [], size = nodes.length;
        for (var j = 0; j < nodes.length; j++) {
          var v = evaluate(preds[i], { node: nodes[j], pos: j + 1, size: size, resolve: ctx.resolve, doc: ctx.doc });
          if (typeof v === 'number' ? v === j + 1 : toBoolean(v)) kept.push(nodes[j]);
        }
        nodes = kept;
      }
      return nodes;
    };
    var applyStep = function (set, step, ctx) {
      var out = [];
      for (var i = 0; i < set.length; i++) {
        var cand = axisNodes(set[i], step.axis).filter(function (n) { return testNode(n, step, ctx); });
        cand = evalPreds(cand, step.preds, ctx);   // (reverse axes are in reverse document order here, which is their proximity order)
        for (var k = 0; k < cand.length; k++) out.push(cand[k]);
      }
      return order(out);
    };
    var compare = function (op, a, b) {
      var cmp = function (x, y) {
        if (op === '=' || op === '!=') {
          var eq;
          if (typeof x === 'boolean' || typeof y === 'boolean') eq = toBoolean(x) === toBoolean(y);
          else if (typeof x === 'number' || typeof y === 'number') eq = toNumber(x) === toNumber(y);
          else eq = toString(x) === toString(y);
          return op === '=' ? eq : !eq;
        }
        var nx = toNumber(x), ny = toNumber(y);
        return op === '<' ? nx < ny : op === '>' ? nx > ny : op === '<=' ? nx <= ny : nx >= ny;
      };
      var sa = isNodeSet(a), sb = isNodeSet(b);
      if (sa && sb) { for (var i = 0; i < a.length; i++) for (var j = 0; j < b.length; j++) if (cmp(strValue(a[i]), strValue(b[j]))) return true; return false; }
      if (sa) { if (typeof b === 'boolean') return cmp(toBoolean(a), b); for (var k = 0; k < a.length; k++) if (cmp(strValue(a[k]), b)) return true; return false; }
      if (sb) { if (typeof a === 'boolean') return cmp(a, toBoolean(b)); for (var m = 0; m < b.length; m++) if (cmp(a, strValue(b[m]))) return true; return false; }
      return cmp(a, b);
    };
    var round = function (n) { return n !== n || n === Infinity || n === -Infinity ? n : Math.floor(n + 0.5); };
    var nodeName = function (n) { return isAttr(n) ? n.name : n.nodeType === 1 ? (htmlDoc(docOf(n)) ? n.nodeName.toLowerCase() : n.nodeName) : n.nodeType === 7 ? n.target : ''; };
    var translate = function (s, from, to) { var o = ''; for (var i = 0; i < s.length; i++) { var at = from.indexOf(s.charAt(i)); if (at < 0) o += s.charAt(i); else if (at < to.length) o += to.charAt(at); } return o; };
    var functions = {
      last: function (c) { return c.size; }, position: function (c) { return c.pos; },
      count: function (c, a) { if (!isNodeSet(a[0])) throw xerr('count() needs a node-set.'); return a[0].length; },
      id: function (c, a) {
        var ids = (isNodeSet(a[0]) ? a[0].map(strValue).join(' ') : toString(a[0])).split(/\s+/).filter(Boolean), out = [];
        ids.forEach(function (i) { var e = docOf(c.node).getElementById(i); if (e) out.push(e); });
        return order(out);
      },
      'local-name': function (c, a) { var n = a.length ? a[0][0] : c.node; return n ? (isAttr(n) ? n.localName : n.nodeType === 1 ? (htmlDoc(docOf(n)) ? n.localName.toLowerCase() : n.localName) : n.nodeType === 7 ? n.target : '') : ''; },
      'namespace-uri': function (c, a) { var n = a.length ? a[0][0] : c.node; return n && n.namespaceURI ? n.namespaceURI : ''; },
      name: function (c, a) { var n = a.length ? a[0][0] : c.node; return n ? nodeName(n) : ''; },
      string: function (c, a) { return a.length ? toString(a[0]) : strValue(c.node); },
      concat: function (c, a) { return a.map(toString).join(''); },
      'starts-with': function (c, a) { return toString(a[0]).indexOf(toString(a[1])) === 0; },
      contains: function (c, a) { return toString(a[0]).indexOf(toString(a[1])) >= 0; },
      'substring-before': function (c, a) { var s = toString(a[0]), i = s.indexOf(toString(a[1])); return i < 0 ? '' : s.slice(0, i); },
      'substring-after': function (c, a) { var s = toString(a[0]), t = toString(a[1]), i = s.indexOf(t); return i < 0 ? '' : s.slice(i + t.length); },
      substring: function (c, a) {
        var s = toString(a[0]), start = round(toNumber(a[1])), len = a.length > 2 ? round(toNumber(a[2])) : Infinity, out = '';
        for (var i = 0; i < s.length; i++) { var p = i + 1; if (p >= start && p < start + len) out += s.charAt(i); }
        return out;
      },
      'string-length': function (c, a) { return (a.length ? toString(a[0]) : strValue(c.node)).length; },
      'normalize-space': function (c, a) { return (a.length ? toString(a[0]) : strValue(c.node)).trim().replace(/\s+/g, ' '); },
      translate: function (c, a) { return translate(toString(a[0]), toString(a[1]), toString(a[2])); },
      boolean: function (c, a) { return toBoolean(a[0]); }, not: function (c, a) { return !toBoolean(a[0]); }, 'true': function () { return true; }, 'false': function () { return false; },
      lang: function (c, a) {
        var want = toString(a[0]).toLowerCase();
        for (var n = isAttr(c.node) ? c.node.ownerElement : c.node; n && n.nodeType === 1; n = n.parentNode) {
          var l = n.getAttribute('xml:lang') || n.getAttribute('lang');
          if (l !== null) { l = l.toLowerCase(); return l === want || l.indexOf(want + '-') === 0; }
        }
        return false;
      },
      number: function (c, a) { return a.length ? toNumber(a[0]) : toNumber(strValue(c.node)); },
      sum: function (c, a) { var s = 0; a[0].forEach(function (n) { s += toNumber(strValue(n)); }); return s; },
      floor: function (c, a) { return Math.floor(toNumber(a[0])); }, ceiling: function (c, a) { return Math.ceil(toNumber(a[0])); }, round: function (c, a) { return round(toNumber(a[0])); }
    };
    var arity = { last: [0, 0], position: [0, 0], count: [1, 1], id: [1, 1], 'local-name': [0, 1], 'namespace-uri': [0, 1], name: [0, 1], string: [0, 1], concat: [2, 1e9], 'starts-with': [2, 2], contains: [2, 2],
                  'substring-before': [2, 2], 'substring-after': [2, 2], substring: [2, 3], 'string-length': [0, 1], 'normalize-space': [0, 1], translate: [3, 3], 'boolean': [1, 1], not: [1, 1], 'true': [0, 0],
                  'false': [0, 0], lang: [1, 1], number: [0, 1], sum: [1, 1], floor: [1, 1], ceiling: [1, 1], round: [1, 1] };
    var check = function (e) {   // function names and their argument counts are known when the expression is made
      if (!e || typeof e !== 'object') return;
      if (e.k === 'call') {
        var ar = arity[e.n];
        if (!ar) throw xerr("The function '" + e.n + "' is not a function of XPath 1.0.", 'NamespaceError');
        if (e.args.length < ar[0] || e.args.length > ar[1]) throw xerr("Wrong number of arguments for '" + e.n + "'.");
      }
      ['a', 'b', 'from'].forEach(function (k) { if (e[k]) check(e[k]); });
      (e.args || []).forEach(check); (e.preds || []).forEach(check);
      (e.steps || []).forEach(function (s) { (s.preds || []).forEach(check); });
    };
    function evaluate(e, ctx) {
      switch (e.k) {
        case 'num': return e.v;
        case 'str': return e.v;
        case 'or': return toBoolean(evaluate(e.a, ctx)) || toBoolean(evaluate(e.b, ctx));
        case 'and': return toBoolean(evaluate(e.a, ctx)) && toBoolean(evaluate(e.b, ctx));
        case 'cmp': return compare(e.o, evaluate(e.a, ctx), evaluate(e.b, ctx));
        case 'arith': {
          var x = toNumber(evaluate(e.a, ctx)), y = toNumber(evaluate(e.b, ctx));
          return e.o === '+' ? x + y : e.o === '-' ? x - y : e.o === '*' ? x * y : e.o === 'div' ? x / y : (y === 0 ? NaN : x % y);
        }
        case 'neg': return -toNumber(evaluate(e.a, ctx));
        case 'union': {
          var a = evaluate(e.a, ctx), b = evaluate(e.b, ctx);
          if (!isNodeSet(a) || !isNodeSet(b)) throw xerr('The operands of | must be node-sets.');
          return order(a.concat(b));
        }
        case 'filter': {
          var base = evaluate(e.a, ctx);
          if (!isNodeSet(base)) throw xerr('Only node-sets can be filtered.');
          return evalPreds(order(base), e.preds, ctx);
        }
        case 'call': return functions[e.n](ctx, e.args.map(function (arg) { return evaluate(arg, ctx); }));
        case 'path': {
          var set;
          if (e.from) { set = evaluate(e.from, ctx); if (!isNodeSet(set)) throw xerr('Only node-sets can have steps.'); }
          else if (e.root) set = [docOf(ctx.node)];
          else set = [ctx.node];
          for (var i = 0; i < e.steps.length; i++) set = applyStep(set, e.steps[i], ctx);
          return set;
        }
      }
      throw xerr('Unsupported XPath expression.');
    }
    // --- result
    var makeResult = function (value, type) {
      var r = Object.create(XPathResult.prototype), isSet = isNodeSet(value);
      if (type === T.ANY) type = isSet ? T.UNORDERED_NODE_ITERATOR : typeof value === 'number' ? T.NUMBER : typeof value === 'string' ? T.STRING : T.BOOLEAN;
      if (type >= T.UNORDERED_NODE_ITERATOR && !isSet) throw xerr('The result is not a node set, and so cannot be converted to the desired type.', 'TypeError');
      var state = { type: type, value: value, pos: 0, valid: true };
      hide(r, '__x', state);
      return r;
    };
    defineGetter(XPathResult.prototype, 'resultType', function () { return this.__x.type; });
    var scalar = function (name, types, make) {
      defineGetter(XPathResult.prototype, name, function () {
        var s = this.__x;
        if (types.indexOf(s.type) < 0) throw xerr("Failed to read the '" + name + "' property from 'XPathResult': The result type is not " + name.replace('Value', '') + ".", 'TypeError');
        return make(s.value);
      });
    };
    scalar('numberValue', [T.NUMBER], function (v) { return v; });
    scalar('stringValue', [T.STRING], function (v) { return v; });
    scalar('booleanValue', [T.BOOLEAN], function (v) { return v; });
    scalar('singleNodeValue', [T.ANY_UNORDERED_NODE, T.FIRST_ORDERED_NODE], function (v) { return v.length ? order(v)[0] : null; });
    scalar('snapshotLength', [T.UNORDERED_NODE_SNAPSHOT, T.ORDERED_NODE_SNAPSHOT], function (v) { return v.length; });
    defineGetter(XPathResult.prototype, 'invalidIteratorState', function () { return (this.__x.type === T.UNORDERED_NODE_ITERATOR || this.__x.type === T.ORDERED_NODE_ITERATOR) && !this.__x.valid; });
    method(XPathResult.prototype, 'iterateNext', function () {
      var s = this.__x;
      if (s.type !== T.UNORDERED_NODE_ITERATOR && s.type !== T.ORDERED_NODE_ITERATOR) throw xerr("Failed to execute 'iterateNext' on 'XPathResult': The result type is not an iterator.", 'TypeError');
      if (!s.valid) throw xerr("Failed to execute 'iterateNext' on 'XPathResult': The document has been mutated since the result was returned.", 'InvalidStateError');
      return s.pos < s.value.length ? s.value[s.pos++] : null;
    });
    method(XPathResult.prototype, 'snapshotItem', function (i) {
      var s = this.__x;
      if (s.type !== T.UNORDERED_NODE_SNAPSHOT && s.type !== T.ORDERED_NODE_SNAPSHOT) throw xerr("Failed to execute 'snapshotItem' on 'XPathResult': The result type is not a snapshot.", 'TypeError');
      return s.value[i >>> 0] || null;
    });
    // namespace resolvers: a function, an object with lookupNamespaceURI, or (createNSResolver) the in-scope namespaces of a node
    var resolverOf = function (r) {
      if (r === null || r === undefined) return function () { return null; };
      if (typeof r === 'function') return r;
      if (typeof r.lookupNamespaceURI === 'function') return function (p) { return r.lookupNamespaceURI(p); };
      return function () { return null; };
    };
    var compile = function (src, resolver, doc2) {
      var ast = parse(String(src));
      check(ast);
      var ex = Object.create(XPathExpression.prototype);
      hide(ex, '__e', { ast: ast, resolve: resolverOf(resolver), doc: doc2 });
      return ex;
    };
    method(XPathExpression.prototype, 'evaluate', function (context, type, result) {
      if (arguments.length < 1) throw typeErr("execute 'evaluate' on 'XPathExpression'", '1 argument required, but only 0 present.');
      if (!context || typeof context.nodeType !== 'number') throw typeErr("execute 'evaluate' on 'XPathExpression'", "parameter 1 is not of type 'Node'.");
      var e = this.__e;
      var value = evaluate(e.ast, { node: context, pos: 1, size: 1, resolve: e.resolve, doc: e.doc });
      if (isNodeSet(value)) value = order(value);
      var want = type === undefined ? 0 : type >>> 0;
      if (isNodeSet(value) && want >= 1 && want <= 3) value = want === 1 ? toNumber(value) : want === 2 ? toString(value) : toBoolean(value);
      else if (!isNodeSet(value) && want === 1) value = toNumber(value);
      else if (!isNodeSet(value) && want === 2) value = toString(value);
      else if (!isNodeSet(value) && want === 3) value = toBoolean(value);
      return makeResult(value, want);
    });
    var createNSResolver = function (node) {
      var r = Object.create(XPathNSResolver.prototype);
      r.lookupNamespaceURI = function (prefix) { return node && node.lookupNamespaceURI ? node.lookupNamespaceURI(prefix === '' ? null : prefix) : null; };
      return r;
    };
    method(DP, 'createExpression', function (expr, resolver) {
      if (arguments.length < 1) throw typeErr("execute 'createExpression' on 'Document'", '1 argument required, but only 0 present.');
      return compile(expr, resolver, this);
    });
    method(DP, 'createNSResolver', function (node) { return createNSResolver(node); });
    method(DP, 'evaluate', function (expr, context, resolver, type, result) {
      if (arguments.length < 2) throw typeErr("execute 'evaluate' on 'Document'", '2 arguments required, but only ' + arguments.length + ' present.');
      return compile(expr, resolver, this).evaluate(context, type, result);
    });
    method(XPathEvaluator.prototype, 'createExpression', function (expr, resolver) { return compile(expr, resolver, g.document); });
    method(XPathEvaluator.prototype, 'createNSResolver', function (node) { return createNSResolver(node); });
    method(XPathEvaluator.prototype, 'evaluate', function (expr, context, resolver, type, result) { return compile(expr, resolver, g.document).evaluate(context, type, result); });
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- XML documents, DOMParser and XMLSerializer for them
  (function () {
    var XMLDocument = klass('XMLDocument', g.Document);
    XMLDocument.prototype = Object.create(DP);
    Object.defineProperty(XMLDocument.prototype, 'constructor', { value: XMLDocument, writable: true, configurable: true });
    tag(XMLDocument, 'XMLDocument');
    var NativeParser = g.DOMParser, parserProto = NativeParser.prototype, nativeParse = parserProto.parseFromString;
    var XHTML = 'http://www.w3.org/1999/xhtml', SVG = 'http://www.w3.org/2000/svg', XMLNS = 'http://www.w3.org/2000/xmlns/', XML = 'http://www.w3.org/XML/1998/namespace';
    var xmlTypes = { 'text/xml': 1, 'application/xml': 1, 'application/xhtml+xml': 1, 'image/svg+xml': 1 };
    var emptyDocument = function (type) {
      var d = nativeParse.call(new NativeParser(), '<!DOCTYPE html><html></html>', 'text/html');
      while (d.firstChild) d.removeChild(d.firstChild);
      Object.setPrototypeOf(d, XMLDocument.prototype);
      __set_content_type(d, type);
      return d;
    };
    var ParseError = function (message, position) { this.message = message; this.position = position; };
    var isNameStart = /[A-Za-z_:À-￿]/, isNameChar = /[A-Za-z0-9_:.\-·À-￿]/;
    var decode = function (text, err) {
      return text.replace(/&(#x[0-9A-Fa-f]+|#[0-9]+|[A-Za-z][A-Za-z0-9]*);/g, function (m, name) {
        if (name.charAt(0) === '#') {
          var cp = name.charAt(1) === 'x' ? parseInt(name.slice(2), 16) : parseInt(name.slice(1), 10);
          if (!(cp > 0 && cp <= 0x10FFFF) || (cp >= 0xD800 && cp <= 0xDFFF)) throw new ParseError('Invalid character reference ' + m, 0);
          return String.fromCodePoint ? String.fromCodePoint(cp) : String.fromCharCode(cp);
        }
        var v = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" }[name];
        if (v === undefined) throw new ParseError('Entity \'' + name + '\' not defined', 0);
        return v;
      });
    };
    // Reads the text into a tree of plain objects, then builds nodes from it.
    var readXml = function (src) {
      var i = 0, n = src.length, root = null, stack = [], docLevel = { children: [] }, top = docLevel, doctype = null;
      var fail = function (msg) { throw new ParseError(msg, i); };
      if (src.charCodeAt(0) === 0xFEFF) i = 1;
      var name = function () {
        if (!isNameStart.test(src.charAt(i))) fail('Name expected');
        var s = i; i++;
        while (i < n && isNameChar.test(src.charAt(i))) i++;
        return src.slice(s, i);
      };
      var space = function () { while (i < n && /\s/.test(src.charAt(i))) i++; };
      if (src.slice(i, i + 5) === '<?xml' && /\s/.test(src.charAt(i + 5))) { var e0 = src.indexOf('?>', i); if (e0 < 0) fail('Unterminated XML declaration'); i = e0 + 2; }
      while (i < n) {
        if (src.charAt(i) !== '<') {
          var s = i;
          while (i < n && src.charAt(i) !== '<') i++;
          var text = src.slice(s, i);
          if (text.indexOf(']]>') >= 0) fail("']]>' not allowed in text");
          if (stack.length === 0) { if (/\S/.test(text)) fail('Text outside of the root element'); continue; }
          top.children.push({ k: 'text', v: decode(text) });
          continue;
        }
        if (src.substr(i, 4) === '<!--') {
          var e = src.indexOf('-->', i + 4);
          if (e < 0) fail('Unterminated comment');
          if (src.slice(i + 4, e).indexOf('--') >= 0) fail("'--' not allowed in a comment");
          top.children.push({ k: 'comment', v: src.slice(i + 4, e) }); i = e + 3; continue;
        }
        if (src.substr(i, 9) === '<![CDATA[') {
          var e2 = src.indexOf(']]>', i);
          if (e2 < 0 || stack.length === 0) fail('Unexpected CDATA section');
          top.children.push({ k: 'cdata', v: src.slice(i + 9, e2) }); i = e2 + 3; continue;
        }
        if (src.substr(i, 2) === '<?') {
          i += 2; var target = name();
          if (target.toLowerCase() === 'xml') fail('XML declaration must be at the start of the document');
          var e3 = src.indexOf('?>', i);
          if (e3 < 0) fail('Unterminated processing instruction');
          top.children.push({ k: 'pi', target: target, v: src.slice(i, e3).replace(/^\s+/, '') }); i = e3 + 2; continue;
        }
        if (src.substr(i, 9) === '<!DOCTYPE') {
          if (root || stack.length) fail('Unexpected DOCTYPE');
          var depth = 0, j = i + 9, q = '';
          for (; j < n; j++) { var cj = src.charAt(j); if (q) { if (cj === q) q = ''; } else if (cj === '"' || cj === "'") q = cj; else if (cj === '[') depth++; else if (cj === ']') depth--; else if (cj === '>' && depth <= 0) break; }
          if (j >= n) fail('Unterminated DOCTYPE');
          var m = /^\s*([^\s\[>]+)/.exec(src.slice(i + 9, j));
          doctype = { name: m ? m[1] : '' };
          i = j + 1; continue;
        }
        if (src.substr(i, 2) === '</') {
          i += 2; var close = name(); space();
          if (src.charAt(i) !== '>') fail("'>' expected");
          i++;
          if (!stack.length || stack[stack.length - 1].name !== close) fail('Opening and ending tag mismatch: ' + (stack.length ? stack[stack.length - 1].name : 'none') + ' and ' + close);
          stack.pop();
          top = stack.length ? stack[stack.length - 1] : docLevel;
          continue;
        }
        // a start tag
        i++;
        var tagName = name(), attrs = [], seen = {};
        for (;;) {
          space();
          var c = src.charAt(i);
          if (c === '>' || c === '/') break;
          if (i >= n) fail('Unterminated start tag');
          var an = name(); space();
          if (src.charAt(i) !== '=') fail("'=' expected after attribute name");
          i++; space();
          var q2 = src.charAt(i);
          if (q2 !== '"' && q2 !== "'") fail('Attribute value must be quoted');
          var end = src.indexOf(q2, i + 1);
          if (end < 0) fail('Unterminated attribute value');
          var raw = src.slice(i + 1, end);
          if (raw.indexOf('<') >= 0) fail("'<' not allowed in attribute value");
          if (seen[an]) fail('Attribute ' + an + ' redefined');
          seen[an] = true;
          attrs.push({ name: an, value: decode(raw).replace(/[\t\n\r]/g, ' ') });
          i = end + 1;
        }
        var el = { k: 'el', name: tagName, attrs: attrs, children: [] };
        var selfClosing = src.charAt(i) === '/';
        if (selfClosing) { i++; if (src.charAt(i) !== '>') fail("'>' expected"); }
        i++;
        if (stack.length === 0 && root) fail('Extra content at the end of the document');
        if (stack.length === 0) root = el;
        top.children.push(el);
        if (!selfClosing) { stack.push(el); top = el; }
      }
      if (stack.length) fail('Premature end of data: ' + stack[stack.length - 1].name + ' not closed');
      if (!root) fail('Document is empty');
      return { doc: docLevel.children, root: root, doctype: doctype };
    };
    var splitName = function (qn) { var c = qn.indexOf(':'); return c < 0 ? [null, qn] : [qn.slice(0, c), qn.slice(c + 1)]; };
    var build = function (d, spec, scope, type) {
      switch (spec.k) {
        case 'text': return d.createTextNode(spec.v);
        case 'comment': return d.createComment(spec.v);
        case 'cdata': return d.createCDATASection(spec.v);
        case 'pi': return d.createProcessingInstruction(spec.target, spec.v);
      }
      var local = { xml: XML, xmlns: XMLNS }, k;
      for (k in scope) local[k] = scope[k];
      spec.attrs.forEach(function (a) {
        if (a.name === 'xmlns') local[''] = a.value;
        else if (a.name.indexOf('xmlns:') === 0) local[a.name.slice(6)] = a.value;
      });
      var sp = splitName(spec.name), ns = sp[0] === null ? (local[''] === undefined || local[''] === '' ? null : local['']) : local[sp[0]];
      if (sp[0] !== null && ns === undefined) throw new ParseError('Namespace prefix ' + sp[0] + ' on ' + sp[1] + ' is not defined', 0);
      if (ns === null && type === 'image/svg+xml' && sp[0] === null) ns = SVG;
      var el = d.createElementNS(ns, spec.name);
      spec.attrs.forEach(function (a) {
        var ap = splitName(a.name);
        if (a.name === 'xmlns' || ap[0] === 'xmlns') { el.setAttributeNS(XMLNS, a.name, a.value); return; }
        var ans = ap[0] === null ? null : local[ap[0]];
        if (ap[0] !== null && ans === undefined) throw new ParseError('Namespace prefix ' + ap[0] + ' for ' + ap[1] + ' on ' + spec.name + ' is not defined', 0);
        el.setAttributeNS(ans, a.name, a.value);
      });
      spec.children.forEach(function (c) { el.appendChild(build(d, c, local, type)); });
      return el;
    };
    var parseXml = function (src, type) {
      var d = emptyDocument(type);
      try {
        var tree = readXml(src);
        tree.doc.forEach(function (s) { d.appendChild(build(d, s, {}, type)); });
      } catch (e) {
        if (!(e instanceof ParseError)) throw e;
        while (d.firstChild) d.removeChild(d.firstChild);
        var ns = 'http://www.mozilla.org/newlayout/xml/parsererror.xml';
        var pe = d.createElementNS(ns, 'parsererror');
        pe.setAttribute('style', 'display: block; white-space: pre; border: 2px solid #c77; padding: 0 1em 0 1em; margin: 1em; background-color: #fdd; color: black');
        var h3 = d.createElementNS(ns, 'h3'); h3.textContent = 'This page contains the following errors:';
        var div = d.createElementNS(ns, 'div'); div.setAttribute('style', 'font-family:monospace;font-size:12px'); div.textContent = 'error at offset ' + e.position + ': ' + e.message;
        var h32 = d.createElementNS(ns, 'h3'); h32.textContent = 'Below is a rendering of the page up to the first error.';
        pe.appendChild(h3); pe.appendChild(div); pe.appendChild(h32);
        var root = d.createElementNS(null, 'html');
        root.appendChild(pe);
        d.appendChild(root);
      }
      return d;
    };
    method(parserProto, 'parseFromString', function (str, type) {
      if (arguments.length < 2) throw typeErr("execute 'parseFromString' on 'DOMParser'", '2 arguments required, but only ' + arguments.length + ' present.');
      type = String(type);
      if (!xmlTypes[type]) return nativeParse.call(this, str, type);
      return parseXml(String(str), type);
    });
    // new Document(): an empty XML document
    var DocumentCtor = function () {
      if (!(this instanceof DocumentCtor)) throw typeErr("construct 'Document'", "Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return emptyDocument('application/xml');
    };
    Object.defineProperty(DocumentCtor, 'name', { value: 'Document' });
    DocumentCtor.prototype = DP;
    hide(g, 'Document', DocumentCtor);
    // implementation.createDocument builds a document of XML
    var impl = doc.implementation;
    method(Object.getPrototypeOf(impl) === Object.prototype ? impl : Object.getPrototypeOf(impl), 'createDocument', function (ns, qname, doctype) {
      if (arguments.length < 2) throw typeErr("execute 'createDocument' on 'DOMImplementation'", '2 arguments required, but only ' + arguments.length + ' present.');
      var type = ns === XHTML ? 'application/xhtml+xml' : ns === SVG ? 'image/svg+xml' : 'application/xml';
      var d = emptyDocument(type);
      if (qname !== null && qname !== undefined && String(qname) !== '') d.appendChild(d.createElementNS(ns === null || ns === undefined || ns === '' ? null : String(ns), String(qname)));
      return d;
    });

    // --- XMLSerializer: XML for documents of XML, and the elements in them
    var NativeSerializer = g.XMLSerializer, nativeSerialize = NativeSerializer && NativeSerializer.prototype.serializeToString;
    var escText = function (s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); };
    var escAttr = function (s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/\t/g, '&#9;').replace(/\n/g, '&#10;').replace(/\r/g, '&#13;'); };
    var serializeNode = function (node, inherited) {
      switch (node.nodeType) {
        case 9: { var o = ''; for (var c = node.firstChild; c; c = c.nextSibling) o += serializeNode(c, inherited); return o; }
        case 11: { var f = ''; for (var d = node.firstChild; d; d = d.nextSibling) f += serializeNode(d, inherited); return f; }
        case 3: return escText(node.data);
        case 4: return '<![CDATA[' + node.data + ']]>';
        case 8: return '<!--' + node.data + '-->';
        case 7: return '<?' + node.target + ' ' + node.data + '?>';
        case 10: return '<!DOCTYPE ' + node.name + '>';
      }
      var scope = {}, k;
      for (k in inherited) scope[k] = inherited[k];
      var prefix = node.prefix, ns = node.namespaceURI || null, declared = [], key = prefix || '';
      var qname = prefix ? prefix + ':' + node.localName : node.localName;
      var inScope = scope[key];
      if (ns === null) { if (!prefix && inScope !== undefined && inScope !== null && inScope !== '') { declared.push(['xmlns', '']); scope[''] = null; } }
      else if (inScope !== ns) { declared.push([prefix ? 'xmlns:' + prefix : 'xmlns', ns]); scope[key] = ns; }
      var out = '<' + qname;
      var attrs = node.attributes;
      for (var i = 0; i < attrs.length; i++) {
        var a = attrs[i];
        if (a.name === 'xmlns') { if (!declared.some(function (x) { return x[0] === 'xmlns'; }) && scope[''] !== a.value) { declared.push(['xmlns', a.value]); scope[''] = a.value; } continue; }
        if (a.name.indexOf('xmlns:') === 0) { var pf = a.name.slice(6); if (scope[pf] !== a.value) { declared.push([a.name, a.value]); scope[pf] = a.value; } continue; }
      }
      declared.forEach(function (x) { out += ' ' + x[0] + '="' + escAttr(x[1]) + '"'; });
      for (var j = 0; j < attrs.length; j++) {
        var b = attrs[j];
        if (b.name === 'xmlns' || b.name.indexOf('xmlns:') === 0) continue;
        out += ' ' + b.name + '="' + escAttr(b.value) + '"';
      }
      if (!node.firstChild) {
        var voidHtml = ns === XHTML && /^(area|base|br|col|embed|hr|img|input|link|meta|param|source|track|wbr)$/.test(node.localName);
        return out + (voidHtml ? ' />' : ns === XHTML ? '></' + qname + '>' : '/>');
      }
      out += '>';
      for (var ch = node.firstChild; ch; ch = ch.nextSibling) out += serializeNode(ch, scope);
      return out + '</' + qname + '>';
    };
    if (NativeSerializer) {
      method(NativeSerializer.prototype, 'serializeToString', function (node) {
        if (arguments.length < 1) throw typeErr("execute 'serializeToString' on 'XMLSerializer'", '1 argument required, but only 0 present.');
        if (!node || typeof node.nodeType !== 'number') throw typeErr("execute 'serializeToString' on 'XMLSerializer'", "parameter 1 is not of type 'Node'.");
        var d = node.nodeType === 9 ? node : node.ownerDocument;
        if (d && d.contentType !== 'text/html') return serializeNode(node, {});
        return nativeSerialize.call(this, node);
      });
    }
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- the rest of the window's interfaces
  (function () {
    // MediaQueryList (matchMedia's objects have their own prototype)
    var mql = g.matchMedia('all'), mp = Object.getPrototypeOf(mql);
    if (mp && mp !== Object.prototype && !g.MediaQueryList) {
      var MQL = function () { throw new TypeError('Illegal constructor'); };
      MQL.prototype = mp;
      Object.defineProperty(mp, 'constructor', { value: MQL, writable: true, configurable: true });
      tag(MQL, 'MediaQueryList');
      hide(g, 'MediaQueryList', MQL);
    }
    if (!g.MediaQueryListEvent) mkEvent('MediaQueryListEvent', null, { media: '', matches: false });
    // Crypto
    var c = g.crypto;
    if (c && !g.Crypto) {
      var Crypto = klass('Crypto', null);
      if (Object.getPrototypeOf(c) === Object.prototype) { try { Object.setPrototypeOf(c, Crypto.prototype); } catch (e) { } }
      else { var cp = Object.getPrototypeOf(c); Crypto.prototype = cp; Object.defineProperty(cp, 'constructor', { value: Crypto, writable: true, configurable: true }); tag(Crypto, 'Crypto'); }
    }
    if (!g.SubtleCrypto && c && c.subtle) {
      var SubtleCrypto = klass('SubtleCrypto', null);
      if (Object.getPrototypeOf(c.subtle) === Object.prototype) { try { Object.setPrototypeOf(c.subtle, SubtleCrypto.prototype); } catch (e) { } }
    }
    // gamepads: none are connected
    klass('Gamepad', null);
    klass('GamepadButton', null);
    if (!g.GamepadEvent) mkEvent('GamepadEvent', null, { gamepad: null });
    // createImageBitmap, over an offscreen canvas
    if (!g.createImageBitmap && g.OffscreenCanvas) {
      var ImageBitmap = klass('ImageBitmap', null);
      hide(g, 'createImageBitmap', function (source, sx, sy, sw, sh) {
        if (arguments.length < 1) return Promise.reject(typeErr("execute 'createImageBitmap' on 'Window'", '1 argument required, but only 0 present.'));
        return new Promise(function (resolve, reject) {
          var finish = function (draw, w, h) {
            var cropped = typeof sx === 'number' && typeof sy === 'number' && typeof sw === 'number' && typeof sh === 'number';
            if (cropped && (sw === 0 || sh === 0)) { reject(new RangeError("Failed to execute 'createImageBitmap' on 'Window': The crop rect width is 0.")); return; }
            var cw = cropped ? Math.abs(sw) : w, ch = cropped ? Math.abs(sh) : h;
            if (!(cw > 0 && ch > 0)) { reject(new DOMException("Failed to execute 'createImageBitmap' on 'Window': The source image width is 0.", 'InvalidStateError')); return; }
            var oc = new g.OffscreenCanvas(cw, ch), ctx = oc.getContext('2d');
            ctx.save();
            if (cropped) ctx.translate(-Math.min(sx, sx + sw), -Math.min(sy, sy + sh));
            draw(ctx);
            ctx.restore();
            Object.defineProperty(oc, 'close', { value: function () { Object.defineProperty(oc, 'width', { value: 0, configurable: true }); Object.defineProperty(oc, 'height', { value: 0, configurable: true }); }, configurable: true });
            resolve(oc);
          };
          if (source instanceof g.Blob) {
            var url = g.URL.createObjectURL(source), img = new g.Image();
            img.onload = function () { g.URL.revokeObjectURL(url); finish(function (ctx) { ctx.drawImage(img, 0, 0); }, img.naturalWidth, img.naturalHeight); };
            img.onerror = function () { g.URL.revokeObjectURL(url); reject(new DOMException("Failed to execute 'createImageBitmap' on 'Window': The source image could not be decoded.", 'InvalidStateError')); };
            img.src = url;
          } else if (source && source.data && source.width !== undefined && source.height !== undefined && source.colorSpace !== undefined) {
            finish(function (ctx) { ctx.putImageData(source, 0, 0); }, source.width, source.height);
          } else if (source && typeof source === 'object') {
            var w = source.videoWidth || source.naturalWidth || source.width, h = source.videoHeight || source.naturalHeight || source.height;
            finish(function (ctx) { ctx.drawImage(source, 0, 0); }, w, h);
          } else reject(typeErr("execute 'createImageBitmap' on 'Window'", "The provided value is not of type '(Blob or HTMLCanvasElement or HTMLImageElement or HTMLVideoElement or ImageBitmap or ImageData or OffscreenCanvas)'."));
        });
      });
    }
  }());

  // ---------------------------------------------------------------------------------------------------------------------------------- namespaces
  (function () {
    var XMLNS = 'http://www.w3.org/2000/xmlns/', XML = 'http://www.w3.org/XML/1998/namespace';
    var split = function (q) { var c = q.indexOf(':'); return c < 0 ? [null, q] : [q.slice(0, c), q.slice(c + 1)]; };
    var nsOf = function (v) { return v === null || v === undefined || v === '' ? null : String(v); };
    var validName = function (s) { return /^[A-Za-z_:À-￿][A-Za-z0-9_:.\-·À-￿]*$/.test(s); };
    var check = function (ns, qname, what) {
      qname = String(qname);
      if (!validName(qname)) throw new DOMException("Failed to execute '" + what + "' on 'Element': '" + qname + "' is not a valid attribute name.", 'InvalidCharacterError');
      var parts = split(qname);
      if (parts[0] !== null && ns === null) throw new DOMException("Failed to execute '" + what + "' on 'Element': '" + qname + "' has a prefix, but no namespace was given.", 'NamespaceError');
      if (parts[0] === 'xml' && ns !== XML) throw new DOMException("Failed to execute '" + what + "' on 'Element': '" + qname + "' uses the reserved prefix 'xml' with another namespace.", 'NamespaceError');
      if ((parts[0] === 'xmlns' || qname === 'xmlns') !== (ns === XMLNS)) throw new DOMException("Failed to execute '" + what + "' on 'Element': '" + qname + "' and the namespace do not match.", 'NamespaceError');
      return parts;
    };
    method(EP, 'setAttributeNS', function (ns, qname, value) {
      if (arguments.length < 3) throw typeErr("execute 'setAttributeNS' on 'Element'", '3 arguments required, but only ' + arguments.length + ' present.');
      ns = nsOf(ns);
      check(ns, qname, 'setAttributeNS');
      __set_attr_ns(this, ns === null ? '' : ns, String(qname), String(value));
    });
    var find = function (el, ns, local) {
      ns = nsOf(ns); local = String(local);
      var names = el.getAttributeNames();
      for (var i = 0; i < names.length; i++) {
        var parts = split(names[i]);
        if (parts[1] === local && nsOf(__attr_ns(el, names[i])) === ns) return names[i];
      }
      return null;
    };
    method(EP, 'getAttributeNS', function (ns, local) {
      if (arguments.length < 2) throw typeErr("execute 'getAttributeNS' on 'Element'", '2 arguments required, but only ' + arguments.length + ' present.');
      var n = find(this, ns, local);
      return n === null ? null : this.getAttribute(n);
    });
    method(EP, 'hasAttributeNS', function (ns, local) {
      if (arguments.length < 2) throw typeErr("execute 'hasAttributeNS' on 'Element'", '2 arguments required, but only ' + arguments.length + ' present.');
      return find(this, ns, local) !== null;
    });
    method(EP, 'removeAttributeNS', function (ns, local) { var n = find(this, ns, local); if (n !== null) this.removeAttribute(n); });
    method(EP, 'getAttributeNodeNS', function (ns, local) { var n = find(this, ns, local); return n === null ? null : this.getAttributeNode(n); });
    method(EP, 'setAttributeNodeNS', function (attr) { return this.setAttributeNode(attr); });
    // namespace lookup, over the namespaces of the elements and their xmlns attributes
    var lookup = function (node, prefix) {
      if (node.nodeType === 1) {
        if (node.namespaceURI && (node.prefix || null) === prefix) return node.namespaceURI;
        var names = node.getAttributeNames();
        for (var i = 0; i < names.length; i++) {
          if (prefix === null && names[i] === 'xmlns') return node.getAttribute(names[i]) || null;
          if (prefix !== null && names[i] === 'xmlns:' + prefix) return node.getAttribute(names[i]) || null;
        }
        return node.parentElement ? lookup(node.parentElement, prefix) : null;
      }
      if (node.nodeType === 9) return node.documentElement ? lookup(node.documentElement, prefix) : null;
      if (node.nodeType === 10 || node.nodeType === 11) return null;
      return node.parentElement ? lookup(node.parentElement, prefix) : null;
    };
    method(NP, 'lookupNamespaceURI', function (prefix) {
      prefix = nsOf(prefix);
      if (prefix === 'xml') return XML;
      if (prefix === 'xmlns') return XMLNS;
      return lookup(this, prefix);
    });
    var lookupPrefix = function (node, ns, origin) {
      if (node.nodeType === 1) {
        if (node.namespaceURI === ns && node.prefix && lookup(origin, node.prefix) === ns) return node.prefix;
        var names = node.getAttributeNames();
        for (var i = 0; i < names.length; i++) if (names[i].indexOf('xmlns:') === 0 && node.getAttribute(names[i]) === ns && lookup(origin, names[i].slice(6)) === ns) return names[i].slice(6);
        return node.parentElement ? lookupPrefix(node.parentElement, ns, origin) : null;
      }
      if (node.nodeType === 9) return node.documentElement ? lookupPrefix(node.documentElement, ns, origin) : null;
      if (node.nodeType === 10 || node.nodeType === 11) return null;
      return node.parentElement ? lookupPrefix(node.parentElement, ns, origin) : null;
    };
    method(NP, 'lookupPrefix', function (ns) {
      ns = nsOf(ns);
      if (ns === null) return null;
      var start = this.nodeType === 1 || this.nodeType === 9 ? this : this.parentElement;
      return start ? lookupPrefix(start, ns, start.nodeType === 9 ? start.documentElement || start : start) : null;
    });
    method(NP, 'isDefaultNamespace', function (ns) { return nsOf(ns) === (this.nodeType === 11 || this.nodeType === 10 ? null : lookup(this, null)); });
    method(EP, 'getElementsByTagNameNS', function (ns, name) {
      var all = this.getElementsByTagName('*'), out = [];
      for (var i = 0; i < all.length; i++) if ((name === '*' || all[i].localName === name) && (ns === '*' || (nsOf(ns) === null ? !all[i].namespaceURI : all[i].namespaceURI === ns))) out.push(all[i]);
      return out;
    });
  }());

  hide(g, '__doctype_of', function (d) {
    var t = d.__doctype;
    if (!t) { t = d.implementation.createDocumentType('html', '', ''); Object.defineProperty(d, '__doctype', { value: t }); }
    return t;
  });

  // getElementsByTagName in a document of XML matches the qualified name, in the case it was written
  (function () {
    var wrap = function (native) {
      return function (name) {
        if (arguments.length < 1) throw typeErr("execute 'getElementsByTagName'", '1 argument required, but only 0 present.');
        var d = this.nodeType === 9 ? this : this.ownerDocument;
        if (!d || d.contentType === 'text/html') return native.call(this, name);
        name = String(name);
        var all = native.call(this, '*'), out = [];
        for (var i = 0; i < all.length; i++) if (name === '*' || all[i].tagName === name) out.push(all[i]);
        try { Object.setPrototypeOf(out, g.HTMLCollection.prototype); } catch (e) { }
        return out;
      };
    };
    method(DP, 'getElementsByTagName', wrap(DP.getElementsByTagName));
    method(EP, 'getElementsByTagName', wrap(EP.getElementsByTagName));
  }());

  // createEvent: the event of the interface that was asked for
  (function () {
    var interfaces = { event: 'Event', events: 'Event', htmlevents: 'Event', uievent: 'UIEvent', uievents: 'UIEvent', mouseevent: 'MouseEvent', mouseevents: 'MouseEvent', keyboardevent: 'KeyboardEvent',
                       customevent: 'CustomEvent', focusevent: 'FocusEvent', compositionevent: 'CompositionEvent', messageevent: 'MessageEvent', storageevent: 'StorageEvent', dragevent: 'DragEvent',
                       hashchangeevent: 'HashChangeEvent', popstateevent: 'PopStateEvent', textevent: 'UIEvent', touchevent: 'TouchEvent', wheelevent: 'WheelEvent', pointerevent: 'PointerEvent',
                       beforeunloadevent: 'BeforeUnloadEvent', clipboardevent: 'ClipboardEvent', pagetransitionevent: 'PageTransitionEvent', progressevent: 'ProgressEvent', submitevent: 'SubmitEvent',
                       transitionevent: 'TransitionEvent', animationevent: 'AnimationEvent', errorevent: 'ErrorEvent', closeevent: 'CloseEvent', trackevent: 'TrackEvent' };
    var nativeCreate = DP.createEvent;
    method(DP, 'createEvent', function (name) {
      if (arguments.length < 1) throw typeErr("execute 'createEvent' on 'Document'", '1 argument required, but only 0 present.');
      var key = String(name).toLowerCase(), C = interfaces[key] && g[interfaces[key]];
      if (!C) throw new DOMException("Failed to execute 'createEvent' on 'Document': The provided event type ('" + name + "') is invalid.", 'NotSupportedError');
      var e;
      try { e = new C(''); } catch (x) { return nativeCreate ? nativeCreate.call(this, name) : new Event(''); }
      Object.defineProperty(e, '__legacy', { value: true });
      return e;
    });
  }());
  // @@END1
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
