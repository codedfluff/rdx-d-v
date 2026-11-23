// web/prelude.js - standard JavaScript and web features written in
// JavaScript itself, run before each page's scripts (after the built-ins
// written in C). Only what is missing is added. tools/genprelude.py turns
// this file into web/js_prelude.c; run it after changing this file.
(function () {
  "use strict";
  var define = function (o, name, value) {
    if (o && !(name in o)) Object.defineProperty(o, name, { value: value, writable: true, configurable: true, enumerable: false });
  };
  var win = window;

  // ---- numbers
  function shortestDigits(a) { // the digits and exponent of a positive number
    var s = String(a), e = 0, i = s.indexOf("e");
    if (i >= 0) { e = +s.slice(i + 1); s = s.slice(0, i); }
    var p = s.indexOf("."), ip = p < 0 ? s : s.slice(0, p), fp = p < 0 ? "" : s.slice(p + 1);
    var d = ip + fp, lead = 0;
    while (lead < d.length - 1 && d.charAt(lead) === "0") lead++;
    e += ip.length - 1 - lead;
    d = d.slice(lead).replace(/0+$/, "") || "0";
    return [d, e];
  }
  define(Number.prototype, "toExponential", function (fd) {
    var x = +this;
    if (!isFinite(x)) return String(x);
    if (fd !== undefined) {
      fd = Math.floor(+fd) || 0;
      if (fd < 0 || fd > 100) throw new RangeError("toExponential() argument must be between 0 and 100");
    }
    var sign = x < 0 ? "-" : "", d, e;
    x = Math.abs(x);
    if (x === 0) {
      e = 0;
      d = "0" + (fd ? new Array(fd + 1).join("0") : "");
    } else if (fd === undefined) {
      var r = shortestDigits(x);
      d = r[0];
      e = r[1];
    } else {
      e = Math.floor(Math.log(x) / Math.LN10);
      var m = x / Math.pow(10, e);
      if (m >= 10) { m /= 10; e++; } else if (m < 1) { m *= 10; e--; }
      var t = m.toFixed(fd);
      if (+t >= 10) { e++; t = (m / 10).toFixed(fd); }
      d = t.replace(".", "");
    }
    return sign + d.charAt(0) + (d.length > 1 ? "." + d.slice(1) : "") + "e" + (e < 0 ? "-" : "+") + Math.abs(e);
  });
  define(Math, "acosh", function (x) { return Math.log(x + Math.sqrt(x * x - 1)); });
  define(Math, "asinh", function (x) { return x === -Infinity ? x : Math.log(x + Math.sqrt(x * x + 1)); });
  define(Math, "atanh", function (x) { return Math.log((1 + x) / (1 - x)) / 2; });

  // ---- strings
  var html = [["anchor", "a", "name"], ["big", "big"], ["blink", "blink"], ["bold", "b"], ["fixed", "tt"], ["fontcolor", "font", "color"],
              ["fontsize", "font", "size"], ["italics", "i"], ["link", "a", "href"], ["small", "small"], ["strike", "strike"],
              ["sub", "sub"], ["sup", "sup"]];
  html.forEach(function (h) {
    define(String.prototype, h[0], function (v) {
      var open = "<" + h[1] + (h[2] ? " " + h[2] + '="' + String(v).replace(/"/g, "&quot;") + '"' : "") + ">";
      return open + String(this) + "</" + h[1] + ">";
    });
  });
  define(String.prototype, "isWellFormed", function () { return true; }); // (strings here are UTF-8: always well formed)
  define(String.prototype, "toWellFormed", function () { return String(this); });
  define(String, "raw", function (strings) {
    var raw = strings.raw || strings, out = "";
    for (var i = 0; i < raw.length; i++) out += raw[i] + (i + 1 < raw.length && i + 1 < arguments.length ? arguments[i + 1] : "");
    return out;
  });

  // ---- arrays
  define(Array.prototype, "copyWithin", function (target, start, end) {
    var len = this.length >>> 0, rel = function (n, d) { n = n === undefined ? d : Math.trunc(+n) || 0; return n < 0 ? Math.max(len + n, 0) : Math.min(n, len); };
    var to = rel(target, 0), from = rel(start, 0), last = rel(end, len), count = Math.min(last - from, len - to);
    var copy = Array.prototype.slice.call(this, from, from + count);
    for (var i = 0; i < copy.length; i++) this[to + i] = copy[i];
    return this;
  });
  define(Array.prototype, "toReversed", function () { return Array.prototype.slice.call(this).reverse(); });
  define(Array.prototype, "toSorted", function (cmp) { return Array.prototype.slice.call(this).sort(cmp); });
  define(Array.prototype, "toSpliced", function () {
    var a = Array.prototype.slice.call(this);
    Array.prototype.splice.apply(a, arguments);
    return a;
  });
  define(Array.prototype, "with", function (i, v) {
    var a = Array.prototype.slice.call(this);
    i = Math.trunc(+i) || 0;
    if (i < 0) i += a.length;
    if (i < 0 || i >= a.length) throw new RangeError("Invalid index");
    a[i] = v;
    return a;
  });

  // ---- objects
  define(Object, "getOwnPropertyDescriptors", function (o) {
    var out = {};
    Reflect.ownKeys(o).forEach(function (k) {
      var d = Object.getOwnPropertyDescriptor(o, k);
      if (d) out[k] = d;
    });
    return out;
  });
  var groupBy = function (items, key, asMap) {
    var out = asMap ? new Map() : Object.create(null), i = 0;
    for (var item of items) {
      var k = key(item, i++);
      if (asMap) { if (!out.has(k)) out.set(k, []); out.get(k).push(item); }
      else (out[k] || (out[k] = [])).push(item);
    }
    return out;
  };
  define(Object, "groupBy", function (items, key) { return groupBy(items, key, false); });
  define(Map, "groupBy", function (items, key) { return groupBy(items, key, true); });
  define(Object.prototype, "__defineGetter__", function (k, f) { Object.defineProperty(this, k, { get: f, configurable: true, enumerable: true }); });
  define(Object.prototype, "__defineSetter__", function (k, f) { Object.defineProperty(this, k, { set: f, configurable: true, enumerable: true }); });
  define(Object.prototype, "__lookupGetter__", function (k) {
    for (var o = this; o; o = Object.getPrototypeOf(o)) { var d = Object.getOwnPropertyDescriptor(o, k); if (d) return d.get; }
  });
  define(Object.prototype, "__lookupSetter__", function (k) {
    for (var o = this; o; o = Object.getPrototypeOf(o)) { var d = Object.getOwnPropertyDescriptor(o, k); if (d) return d.set; }
  });
  define(Reflect, "isExtensible", function (o) { return Object.isExtensible ? Object.isExtensible(o) : true; });
  define(Reflect, "preventExtensions", function (o) { if (Object.preventExtensions) Object.preventExtensions(o); return true; });
  define(Date.prototype, "setYear", function (y) { y = +y; return this.setFullYear(y >= 0 && y <= 99 ? 1900 + y : y); });
  define(RegExp.prototype, "compile", function () { return this; });

  // ---- iterating web collections
  var USP = URLSearchParams.prototype;
  var pairs = function (sp) { var out = []; sp.forEach(function (v, k) { out.push([k, v]); }); return out; };
  define(USP, "entries", function () { return pairs(this)[Symbol.iterator](); });
  define(USP, "keys", function () { return pairs(this).map(function (p) { return p[0]; })[Symbol.iterator](); });
  define(USP, "values", function () { return pairs(this).map(function (p) { return p[1]; })[Symbol.iterator](); });
  define(USP, Symbol.iterator, USP.entries);
  define(USP, "sort", function () {
    var p = pairs(this).sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }), self = this;
    p.forEach(function (x) { self["delete"](x[0]); });
    p.forEach(function (x) { self.append(x[0], x[1]); });
  });
  if (document.documentElement && document.documentElement.classList) {
    var CL = Object.getPrototypeOf(document.documentElement.classList);
    var tokens = function (list) { var out = []; for (var i = 0; i < list.length; i++) out.push(list.item(i)); return out; };
    define(CL, "values", function () { return tokens(this)[Symbol.iterator](); });
    define(CL, "entries", function () { return tokens(this).map(function (t, i) { return [i, t]; })[Symbol.iterator](); });
    define(CL, "keys", function () { return tokens(this).map(function (t, i) { return i; })[Symbol.iterator](); });
    define(CL, Symbol.iterator, CL.values);
  }

  // ---- web features
  define(win, "WeakRef", function WeakRef(target) { this.deref = function () { return target; }; });
  define(win, "FinalizationRegistry", function FinalizationRegistry() { this.register = function () {}; this.unregister = function () { return false; }; });
  define(win, "Image", function Image(w, h) {
    var img = document.createElement("img");
    if (w !== undefined) img.width = w;
    if (h !== undefined) img.height = h;
    return img;
  });
  define(win, "structuredClone", function structuredClone(v) {
    var seen = new Map();
    return (function copy(x) {
      if (x === null || typeof x !== "object") {
        if (typeof x === "function" || typeof x === "symbol") throw new Error("could not be cloned");
        return x;
      }
      if (seen.has(x)) return seen.get(x);
      var out;
      if (x instanceof Date) out = new Date(x.getTime());
      else if (x instanceof RegExp) out = new RegExp(x.source, x.flags);
      else if (x instanceof Map) { out = new Map(); seen.set(x, out); x.forEach(function (v, k) { out.set(copy(k), copy(v)); }); return out; }
      else if (x instanceof Set) { out = new Set(); seen.set(x, out); x.forEach(function (v) { out.add(copy(v)); }); return out; }
      else if (Array.isArray(x)) { out = []; seen.set(x, out); for (var i = 0; i < x.length; i++) out[i] = copy(x[i]); return out; }
      else { out = {}; seen.set(x, out); Object.keys(x).forEach(function (k) { out[k] = copy(x[k]); }); return out; }
      seen.set(x, out);
      return out;
    })(v);
  });

  function AbortSignal() { this.aborted = false; this.reason = undefined; this.onabort = null; this._l = []; }
  AbortSignal.prototype.addEventListener = function (t, f) { if (t === "abort") this._l.push(f); };
  AbortSignal.prototype.removeEventListener = function (t, f) { this._l = this._l.filter(function (g) { return g !== f; }); };
  AbortSignal.prototype.throwIfAborted = function () { if (this.aborted) throw this.reason; };
  AbortSignal.abort = function (reason) { var c = new AbortController(); c.abort(reason); return c.signal; };
  AbortSignal.timeout = function () { return new AbortSignal(); };
  function AbortController() { this.signal = new AbortSignal(); }
  AbortController.prototype.abort = function (reason) {
    var s = this.signal;
    if (s.aborted) return;
    s.aborted = true;
    s.reason = reason === undefined ? new Error("The operation was aborted.") : reason;
    var e = { type: "abort", target: s };
    if (typeof s.onabort === "function") s.onabort(e);
    s._l.forEach(function (f) { f.call(s, e); });
  };
  define(win, "AbortController", AbortController);
  define(win, "AbortSignal", AbortSignal);

  function Headers(init) {
    this._h = {};
    var self = this;
    if (init instanceof Headers) init.forEach(function (v, k) { self.append(k, v); });
    else if (Array.isArray(init)) init.forEach(function (p) { self.append(p[0], p[1]); });
    else if (init) Object.keys(init).forEach(function (k) { self.append(k, init[k]); });
  }
  Headers.prototype.append = function (k, v) { k = String(k).toLowerCase(); this._h[k] = k in this._h ? this._h[k] + ", " + v : String(v); };
  Headers.prototype.set = function (k, v) { this._h[String(k).toLowerCase()] = String(v); };
  Headers.prototype.get = function (k) { k = String(k).toLowerCase(); return k in this._h ? this._h[k] : null; };
  Headers.prototype.has = function (k) { return String(k).toLowerCase() in this._h; };
  Headers.prototype["delete"] = function (k) { delete this._h[String(k).toLowerCase()]; };
  Headers.prototype.forEach = function (f, t) { for (var k in this._h) f.call(t, this._h[k], k, this); };
  Headers.prototype.entries = function () { var h = this._h; return Object.keys(h).map(function (k) { return [k, h[k]]; })[Symbol.iterator](); };
  Headers.prototype.keys = function () { return Object.keys(this._h)[Symbol.iterator](); };
  Headers.prototype.values = function () { var h = this._h; return Object.keys(h).map(function (k) { return h[k]; })[Symbol.iterator](); };
  Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
  define(win, "Headers", Headers);

  function FormData(form) {
    this._e = [];
    if (form && form.elements) {
      for (var i = 0; i < form.elements.length; i++) {
        var el = form.elements[i], type = (el.type || "").toLowerCase();
        if (!el.name || el.disabled || type === "submit" || type === "button" || type === "file" || type === "reset") continue;
        if ((type === "checkbox" || type === "radio") && !el.checked) continue;
        this._e.push([el.name, el.value == null ? "" : String(el.value)]);
      }
    }
  }
  FormData.prototype.append = function (k, v) { this._e.push([String(k), v instanceof Blob ? v : String(v)]); };
  FormData.prototype.set = function (k, v) { this["delete"](k); this.append(k, v); };
  FormData.prototype.get = function (k) { for (var i = 0; i < this._e.length; i++) if (this._e[i][0] === String(k)) return this._e[i][1]; return null; };
  FormData.prototype.getAll = function (k) { return this._e.filter(function (p) { return p[0] === String(k); }).map(function (p) { return p[1]; }); };
  FormData.prototype.has = function (k) { return this.get(k) !== null; };
  FormData.prototype["delete"] = function (k) { this._e = this._e.filter(function (p) { return p[0] !== String(k); }); };
  FormData.prototype.forEach = function (f, t) { var self = this; this._e.forEach(function (p) { f.call(t, p[1], p[0], self); }); };
  FormData.prototype.entries = function () { return this._e.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); };
  FormData.prototype.keys = function () { return this._e.map(function (p) { return p[0]; })[Symbol.iterator](); };
  FormData.prototype.values = function () { return this._e.map(function (p) { return p[1]; })[Symbol.iterator](); };
  FormData.prototype[Symbol.iterator] = FormData.prototype.entries;
  // (sent by fetch / XMLHttpRequest as a form: name=value&...)
  FormData.prototype.toString = function () {
    return this._e.map(function (p) { return encodeURIComponent(p[0]) + "=" + encodeURIComponent(String(p[1])); }).join("&");
  };
  define(win, "FormData", FormData);

  var rawBytes = win.__rdxBytes, rawBuffer = win.__rdxBuffer;
  delete win.__rdxBytes;
  delete win.__rdxBuffer;
  function Blob(parts, options) { // (the bytes are kept as a string of them)
    var text = "";
    (parts || []).forEach(function (p) {
      text += p instanceof Blob ? p._text : typeof p === "object" && p !== null && (p instanceof ArrayBuffer || ArrayBuffer.isView(p)) ? rawBytes(p) : String(p);
    });
    Object.defineProperty(this, "_text", { value: text });
    this.size = text.length;
    this.type = options && options.type ? String(options.type).toLowerCase() : "";
  }
  Blob.prototype.text = function () { return Promise.resolve(this._text); };
  Blob.prototype.arrayBuffer = function () { return Promise.resolve(rawBuffer(this._text)); };
  Blob.prototype.bytes = function () { return Promise.resolve(new Uint8Array(rawBuffer(this._text))); };
  Blob.prototype.slice = function (a, b, type) { return new Blob([this._text.slice(a, b)], { type: type }); };
  define(win, "Blob", Blob);
  function File(parts, name, options) {
    Blob.call(this, parts, options);
    this.name = String(name);
    this.lastModified = options && options.lastModified || Date.now();
  }
  File.prototype = Object.create(Blob.prototype);
  File.prototype.constructor = File;
  define(win, "File", File);

  define(win, "DOMParser", function DOMParser() {
    this.parseFromString = function (text) {
      var root = document.createElement("html"), body = document.createElement("body");
      root.appendChild(body);
      body.innerHTML = String(text);
      return {
        documentElement: root, body: body, head: null,
        querySelector: function (s) { return root.querySelector(s); },
        querySelectorAll: function (s) { return root.querySelectorAll(s); },
        getElementsByTagName: function (t) { return root.getElementsByTagName(t); },
        getElementById: function (id) { return root.querySelector("#" + id); }
      };
    };
  });

  function MessagePort() { this.onmessage = null; this._other = null; this._l = []; }
  MessagePort.prototype.postMessage = function (data) {
    var to = this._other;
    if (!to) return;
    setTimeout(function () {
      var e = { type: "message", data: data, target: to };
      if (typeof to.onmessage === "function") to.onmessage(e);
      to._l.forEach(function (f) { f.call(to, e); });
    }, 0);
  };
  MessagePort.prototype.addEventListener = function (t, f) { if (t === "message") this._l.push(f); };
  MessagePort.prototype.removeEventListener = function (t, f) { this._l = this._l.filter(function (g) { return g !== f; }); };
  MessagePort.prototype.start = MessagePort.prototype.close = function () {};
  define(win, "MessageChannel", function MessageChannel() {
    this.port1 = new MessagePort();
    this.port2 = new MessagePort();
    this.port1._other = this.port2;
    this.port2._other = this.port1;
  });
  define(win, "BroadcastChannel", function BroadcastChannel(name) {
    this.name = name;
    this.onmessage = null;
    this.postMessage = this.close = this.addEventListener = this.removeEventListener = function () {};
  });

  // ---- performance: the page's timeline (marks, measures, the navigation)
  var perf = win.performance;
  if (perf) {
    var origin = Date.now() - perf.now(), entries = [];
    define(perf, "timeOrigin", origin);
    var navigation = { __proto__: null, name: String(location.href), entryType: "navigation", startTime: 0, duration: 0, initiatorType: "navigation",
      type: "navigate", redirectCount: 0, fetchStart: 0, domainLookupStart: 0, domainLookupEnd: 0, connectStart: 0, connectEnd: 0,
      requestStart: 0, responseStart: 0, responseEnd: 0, domInteractive: 0, domContentLoadedEventStart: 0,
      domContentLoadedEventEnd: 0, domComplete: 0, loadEventStart: 0, loadEventEnd: 0, transferSize: 0, encodedBodySize: 0,
      decodedBodySize: 0, nextHopProtocol: "http/1.1", toJSON: function () { return Object.assign({}, this); } };
    var kind = function (name, parent) {
      var C = function () { throw new TypeError("Illegal constructor"); };
      Object.defineProperty(C, "name", { value: name });
      C.prototype = Object.create(parent ? parent.prototype : Object.prototype);
      C.prototype.constructor = C;
      C.prototype.toJSON = function () { return Object.assign({}, this); };
      define(win, name, C);
      return win[name];
    };
    var PerformanceEntry = kind("PerformanceEntry");
    var PerformanceResourceTiming = kind("PerformanceResourceTiming", PerformanceEntry);
    var PerformanceNavigationTiming = kind("PerformanceNavigationTiming", PerformanceResourceTiming);
    var PerformanceMark = kind("PerformanceMark", PerformanceEntry);
    var PerformanceMeasure = kind("PerformanceMeasure", PerformanceEntry);
    kind("PerformancePaintTiming", PerformanceEntry);
    var made = function (C, fields) { var e = Object.create(C.prototype); Object.assign(e, fields); return e; };
    // what the page loaded: its scripts, stylesheets and pictures
    var resources = function () {
      var out = [], seen = {};
      if (typeof document === "undefined") return out;
      var nodes = document.querySelectorAll("script[src],link[rel~=stylesheet][href],img[src]");
      for (var i = 0; i < nodes.length; i++) {
        var n = nodes[i], url = n.src || n.href, tag = n.tagName.toLowerCase();
        if (!url || seen[url] || /^(data|blob):/.test(url)) continue;
        seen[url] = 1;
        out.push(made(PerformanceResourceTiming, { name: url, entryType: "resource", initiatorType: tag === "img" ? "img" : tag === "link" ? "link" : "script",
          startTime: 0, duration: 0, fetchStart: 0, responseEnd: 0, transferSize: 0, encodedBodySize: 0, decodedBodySize: 0,
          nextHopProtocol: "http/1.1", renderBlockingStatus: "non-blocking", responseStatus: 200 }));
      }
      return out;
    };
    Object.setPrototypeOf(navigation, PerformanceNavigationTiming.prototype);
    var list = function (type, name) {
      var all = [navigation].concat(resources(), entries);
      return all.filter(function (e) { return (!type || e.entryType === type) && (!name || e.name === name); });
    };
    define(perf, "getEntries", function () { return list(); });
    define(perf, "getEntriesByType", function (t) { return list(String(t)); });
    define(perf, "getEntriesByName", function (n, t) { return list(t && String(t), String(n)); });
    define(perf, "mark", function (name, opts) {
      var e = made(PerformanceMark, { name: String(name), entryType: "mark", startTime: opts && opts.startTime !== undefined ? +opts.startTime : perf.now(),
        duration: 0, detail: opts && opts.detail !== undefined ? opts.detail : null });
      entries.push(e);
      return e;
    });
    var at = function (m, fallback) {
      if (m === undefined) return fallback;
      if (typeof m === "number") return m;
      var found = list("mark", String(m));
      if (!found.length) throw new SyntaxError("The mark '" + m + "' does not exist.");
      return found[found.length - 1].startTime;
    };
    define(perf, "measure", function (name, start, end) {
      if (start && typeof start === "object") { end = start.end; start = start.start; }
      var s = at(start, 0), f = at(end, perf.now());
      var e = made(PerformanceMeasure, { name: String(name), entryType: "measure", startTime: s, duration: f - s, detail: null });
      entries.push(e);
      return e;
    });
    var clear = function (type) {
      return function (name) { entries = entries.filter(function (e) { return e.entryType !== type || (name !== undefined && e.name !== name); }); };
    };
    define(perf, "clearMarks", clear("mark"));
    define(perf, "clearMeasures", clear("measure"));
    define(perf, "clearResourceTimings", function () {});
    define(perf, "setResourceTimingBufferSize", function () {});
    define(perf, "toJSON", function () { return { timeOrigin: origin }; });
    var t = Math.round(origin);
    define(perf, "timing", { navigationStart: t, fetchStart: t, domainLookupStart: t, domainLookupEnd: t, connectStart: t, connectEnd: t,
      requestStart: t, responseStart: t, responseEnd: t, domLoading: t, domInteractive: 0, domContentLoadedEventStart: 0,
      domContentLoadedEventEnd: 0, domComplete: 0, loadEventStart: 0, loadEventEnd: 0, unloadEventStart: 0, unloadEventEnd: 0,
      redirectStart: 0, redirectEnd: 0, secureConnectionStart: t });
    define(perf, "navigation", { type: 0, redirectCount: 0, TYPE_NAVIGATE: 0, TYPE_RELOAD: 1, TYPE_BACK_FORWARD: 2 });
    var done = function () {
      var now = perf.now();
      navigation.domInteractive = navigation.domContentLoadedEventStart = navigation.domContentLoadedEventEnd = now;
      perf.timing.domInteractive = perf.timing.domContentLoadedEventStart = perf.timing.domContentLoadedEventEnd = Math.round(origin + now);
    };
    document.addEventListener("DOMContentLoaded", done);
    win.addEventListener("load", function () {
      var now = perf.now();
      navigation.domComplete = navigation.loadEventStart = navigation.loadEventEnd = navigation.duration = now;
      perf.timing.domComplete = perf.timing.loadEventStart = perf.timing.loadEventEnd = Math.round(origin + now);
    });
  }

  // ---- DOM traversal: NodeIterator, TreeWalker
  var NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2,
    SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_PROCESSING_INSTRUCTION: 64, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256,
    SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024 };
  define(win, "NodeFilter", NodeFilter);
  var accepts = function (w, node) {
    var show = w.whatToShow >>> 0, type = node.nodeType;
    if (!(show & (1 << (type - 1)))) return 3;
    var f = w.filter;
    if (!f) return 1;
    var r = typeof f === "function" ? f(node) : f.acceptNode(node);
    return r === undefined ? 1 : +r;
  };
  var following = function (node, root) { // the next node in document order, within root
    if (node.firstChild) return node.firstChild;
    while (node && node !== root) {
      if (node.nextSibling) return node.nextSibling;
      node = node.parentNode;
    }
    return null;
  };
  var preceding = function (node, root) {
    if (node === root) return null;
    if (node.previousSibling) {
      node = node.previousSibling;
      while (node.lastChild) node = node.lastChild;
      return node;
    }
    return node.parentNode;
  };
  function NodeIterator(root, show, filter) {
    this.root = root;
    this.whatToShow = show === undefined ? 0xFFFFFFFF : show;
    this.filter = filter || null;
    this.referenceNode = root;
    this.pointerBeforeReferenceNode = true;
  }
  NodeIterator.prototype.nextNode = function () {
    var node = this.referenceNode, before = this.pointerBeforeReferenceNode;
    for (;;) {
      if (before) before = false;
      else {
        node = following(node, this.root);
        if (!node) return null;
      }
      if (accepts(this, node) === 1) {
        this.referenceNode = node;
        this.pointerBeforeReferenceNode = false;
        return node;
      }
    }
  };
  NodeIterator.prototype.previousNode = function () {
    var node = this.referenceNode, before = this.pointerBeforeReferenceNode;
    for (;;) {
      if (!before) before = true;
      else {
        node = preceding(node, this.root);
        if (!node) return null;
      }
      if (accepts(this, node) === 1) {
        this.referenceNode = node;
        this.pointerBeforeReferenceNode = true;
        return node;
      }
    }
  };
  NodeIterator.prototype.detach = function () {};
  function TreeWalker(root, show, filter) {
    this.root = root;
    this.whatToShow = show === undefined ? 0xFFFFFFFF : show;
    this.filter = filter || null;
    this.currentNode = root;
  }
  var walkTo = function (w, pick) {
    var n = pick(w.currentNode);
    while (n) {
      var r = accepts(w, n);
      if (r === 1) { w.currentNode = n; return n; }
      n = pick(n);
    }
    return null;
  };
  TreeWalker.prototype.parentNode = function () {
    var root = this.root;
    return walkTo(this, function (n) { return n === root ? null : n.parentNode; });
  };
  TreeWalker.prototype.firstChild = function () { return this._child(true); };
  TreeWalker.prototype.lastChild = function () { return this._child(false); };
  TreeWalker.prototype._child = function (first) {
    var n = first ? this.currentNode.firstChild : this.currentNode.lastChild;
    while (n) {
      var r = accepts(this, n);
      if (r === 1) { this.currentNode = n; return n; }
      n = first ? n.nextSibling : n.previousSibling;
    }
    return null;
  };
  TreeWalker.prototype.nextSibling = function () { return walkTo(this, function (n) { return n.nextSibling; }); };
  TreeWalker.prototype.previousSibling = function () { return walkTo(this, function (n) { return n.previousSibling; }); };
  TreeWalker.prototype.nextNode = function () {
    var root = this.root;
    return walkTo(this, function (n) { return following(n, root); });
  };
  TreeWalker.prototype.previousNode = function () {
    var root = this.root;
    return walkTo(this, function (n) { return preceding(n, root); });
  };
  if (typeof document !== "undefined") {
    var docProto = Object.getPrototypeOf(document);
    define(docProto, "createNodeIterator", function (root, show, filter) { return new NodeIterator(root, show, filter); });
    define(docProto, "createTreeWalker", function (root, show, filter) { return new TreeWalker(root, show, filter); });
    define(win, "NodeIterator", NodeIterator);
    define(win, "TreeWalker", TreeWalker);
    if (typeof HTMLScriptElement === "function") {
      ["async", "defer", "noModule"].forEach(function (name) {
        var attr = name.toLowerCase();
        Object.defineProperty(HTMLScriptElement.prototype, name, {
          get: function () { return this.hasAttribute(attr); },
          set: function (v) { if (v) this.setAttribute(attr, ""); else this.removeAttribute(attr); },
          configurable: true
        });
      });
    }
  }

  // ---- SVG geometry objects (feature tests look for these)
  if (typeof SVGElement === "function") {
    var svgProto = SVGElement.prototype;
    var matrix = function () {
      return { a: 1, b: 0, c: 0, d: 1, e: 0, f: 0,
        multiply: function () { return matrix(); }, inverse: function () { return matrix(); },
        translate: function (x, y) { var m = matrix(); m.e = x || 0; m.f = y || 0; return m; },
        scale: function () { return matrix(); }, rotate: function () { return matrix(); } };
    };
    var point = function () {
      return { x: 0, y: 0, matrixTransform: function (m) { return { x: this.x * m.a + this.y * m.c + m.e, y: this.x * m.b + this.y * m.d + m.f }; } };
    };
    svgProto.createSVGRect = function () { return { x: 0, y: 0, width: 0, height: 0 }; };
    svgProto.createSVGPoint = point;
    svgProto.createSVGMatrix = matrix;
    svgProto.createSVGNumber = function () { return { value: 0 }; };
    svgProto.createSVGLength = function () { return { value: 0, unitType: 1 }; };
    svgProto.getScreenCTM = function () { var r = this.getBoundingClientRect(), m = matrix(); m.e = r.left; m.f = r.top; return m; };
    svgProto.getCTM = function () { return matrix(); };
    svgProto.getBBox = function () { var r = this.getBoundingClientRect(); return { x: 0, y: 0, width: r.width, height: r.height }; };
    svgProto.getTotalLength = function () { return 0; };
    svgProto.getPointAtLength = function () { return point(); };
    if (!("ownerSVGElement" in svgProto))
      Object.defineProperty(svgProto, "ownerSVGElement", { get: function () { var p = this.parentNode; while (p && p.nodeName && p.nodeName.toLowerCase() !== "svg") p = p.parentNode; return p && p.nodeName ? p : null; }, configurable: true });
  }

  // ---- node type numbers
  if (typeof Node === "function") {
    var types = { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, ENTITY_REFERENCE_NODE: 5, ENTITY_NODE: 6,
      PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11, NOTATION_NODE: 12 };
    Object.keys(types).forEach(function (k) { define(Node, k, types[k]); define(Node.prototype, k, types[k]); });
  }

  // ---- style sheets: document.styleSheets, and ones made by scripts
  // (new CSSStyleSheet(), adoptedStyleSheets: shown through a <style>)
  var ruleTexts = function (css) {
    var out = [], depth = 0, start = 0;
    css = String(css).replace(/\/\*[\s\S]*?\*\//g, "");
    for (var i = 0; i < css.length; i++) {
      var c = css.charAt(i);
      if (c === "{") depth++;
      else if (c === "}" && --depth === 0) { var t = css.slice(start, i + 1).trim(); if (t) out.push(t); start = i + 1; }
    }
    return out;
  };
  var ruleList = function (sheet) {
    return ruleTexts(sheet._text()).map(function (t) {
      var brace = t.indexOf("{");
      return { cssText: t, selectorText: t.slice(0, brace).trim(), type: t.charAt(0) === "@" ? 4 : 1, parentStyleSheet: sheet };
    });
  };
  function CSSStyleSheet(opts) {
    this._css = "";
    this.ownerNode = null;
    this.href = null;
    this.disabled = !!(opts && opts.disabled);
    this.media = { mediaText: opts && opts.media ? String(opts.media) : "", length: 0 };
    this.type = "text/css";
    this._styles = [];
  }
  CSSStyleSheet.prototype._text = function () { return this.ownerNode ? this.ownerNode.textContent : this._css; };
  CSSStyleSheet.prototype._set = function (css) {
    if (this.ownerNode) { this.ownerNode.textContent = css; return; }
    this._css = css;
    this._styles.forEach(function (s) { s.textContent = css; });
  };
  Object.defineProperty(CSSStyleSheet.prototype, "cssRules", { get: function () { return ruleList(this); }, configurable: true });
  Object.defineProperty(CSSStyleSheet.prototype, "rules", { get: function () { return ruleList(this); }, configurable: true });
  CSSStyleSheet.prototype.insertRule = function (rule, index) {
    var list = ruleTexts(this._text());
    index = index === undefined ? 0 : index >>> 0;
    if (index > list.length) throw new RangeError("Failed to execute 'insertRule' on 'CSSStyleSheet': The index provided is larger than the number of rules.");
    list.splice(index, 0, String(rule));
    this._set(list.join("\n"));
    return index;
  };
  CSSStyleSheet.prototype.deleteRule = function (index) {
    var list = ruleTexts(this._text());
    list.splice(index >>> 0, 1);
    this._set(list.join("\n"));
  };
  CSSStyleSheet.prototype.addRule = function (sel, body, index) { return this.insertRule(sel + "{" + body + "}", index === undefined ? ruleTexts(this._text()).length : index); };
  CSSStyleSheet.prototype.removeRule = CSSStyleSheet.prototype.deleteRule;
  CSSStyleSheet.prototype.replaceSync = function (css) { this._set(String(css)); };
  CSSStyleSheet.prototype.replace = function (css) { this._set(String(css)); return Promise.resolve(this); };
  define(win, "CSSStyleSheet", CSSStyleSheet);
  define(win, "StyleSheet", CSSStyleSheet);
  if (typeof document !== "undefined") {
    var sheetOf = function (node) {
      if (!node._sheet) {
        Object.defineProperty(node, "_sheet", { value: new CSSStyleSheet(), configurable: true });
        node._sheet.ownerNode = node;
        node._sheet.href = node.tagName === "LINK" ? node.href : null;
      }
      return node._sheet;
    };
    Object.defineProperty(Object.getPrototypeOf(document), "styleSheets", {
      get: function () {
        var list = Array.prototype.map.call(this.querySelectorAll("style,link[rel~=stylesheet]"), sheetOf);
        list.item = function (i) { return this[i] || null; };
        return list;
      }, configurable: true
    });
    if (typeof HTMLStyleElement === "function")
      Object.defineProperty(HTMLStyleElement.prototype, "sheet", { get: function () { return sheetOf(this); }, configurable: true });
    if (typeof HTMLLinkElement === "function")
      Object.defineProperty(HTMLLinkElement.prototype, "sheet", { get: function () { return /stylesheet/.test(this.rel) ? sheetOf(this) : null; }, configurable: true });
    // adoptedStyleSheets: each sheet shown by a <style> in the document or shadow root
    var adopted = function (place) {
      return {
        get: function () { return this._adopted || []; },
        set: function (list) {
          var self = this, at = place(this);
          (this._adoptedStyles || []).forEach(function (s) { if (s.parentNode) s.parentNode.removeChild(s); });
          Object.defineProperty(this, "_adopted", { value: Array.prototype.slice.call(list), configurable: true, writable: true });
          Object.defineProperty(this, "_adoptedStyles", { value: [], configurable: true, writable: true });
          this._adopted.forEach(function (sheet) {
            var s = document.createElement("style");
            s.textContent = sheet._text();
            sheet._styles.push(s);
            at.appendChild(s);
            self._adoptedStyles.push(s);
          });
        },
        configurable: true
      };
    };
    Object.defineProperty(Object.getPrototypeOf(document), "adoptedStyleSheets", adopted(function () { return document.head || document.documentElement; }));
    if (typeof ShadowRoot === "function") Object.defineProperty(ShadowRoot.prototype, "adoptedStyleSheets", adopted(function (r) { return r; }));
  }

  // ---- streams: ReadableStream, WritableStream, TransformStream (and the
  // compression and text ones built on them)
  var zip = win.__rdxZip;
  delete win.__rdxZip;
  var resolved = function (v) { return Promise.resolve(v); };
  var sizeOf = function (strategy) {
    var hwm = strategy && strategy.highWaterMark !== undefined ? +strategy.highWaterMark : 1;
    var size = strategy && typeof strategy.size === "function" ? strategy.size : function () { return 1; };
    return { hwm: hwm, size: size };
  };

  function ReadableStreamDefaultController(stream) { this._s = stream; }
  Object.defineProperty(ReadableStreamDefaultController.prototype, "desiredSize", {
    get: function () { var s = this._s; return s._state === "errored" ? null : s._state === "closed" ? 0 : s._hwm - s._queued; },
    configurable: true
  });
  ReadableStreamDefaultController.prototype.enqueue = function (chunk) {
    var s = this._s;
    if (s._closeRequested || s._state !== "readable") throw new TypeError("Cannot enqueue a chunk into a closed stream");
    if (s._reads.length) s._reads.shift().resolve({ value: chunk, done: false });
    else { s._queue.push(chunk); s._queued += s._size(chunk); }
    s._pullSoon();
  };
  ReadableStreamDefaultController.prototype.close = function () {
    var s = this._s;
    if (s._closeRequested || s._state !== "readable") throw new TypeError("The stream is closed already");
    s._closeRequested = true;
    if (!s._queue.length) s._finish();
  };
  ReadableStreamDefaultController.prototype.error = function (e) { this._s._fail(e); };

  function ReadableStream(source, strategy) {
    if (!(this instanceof ReadableStream)) throw new TypeError("Failed to construct 'ReadableStream': Please use the 'new' operator");
    source = source || {};
    var st = sizeOf(strategy), self = this;
    this._state = "readable";
    this._queue = [];
    this._queued = 0;
    this._reads = [];
    this._hwm = st.hwm;
    this._size = st.size;
    this._source = source;
    this._started = false;
    this._pulling = false;
    this._closeRequested = false;
    this._reader = null;
    this._closedWaits = [];
    var c = this._controller = new ReadableStreamDefaultController(this);
    try {
      resolved(typeof source.start === "function" ? source.start(c) : undefined).then(function () {
        self._started = true;
        self._pullSoon();
      }, function (e) { self._fail(e); });
    } catch (e) { this._fail(e); }
  }
  ReadableStream.prototype._pullSoon = function () {
    var self = this, src = this._source;
    if (!this._started || this._pulling || this._state !== "readable" || this._closeRequested || typeof src.pull !== "function") return;
    if (!this._reads.length && this._hwm - this._queued <= 0) return;
    this._pulling = true;
    resolved().then(function () { return src.pull(self._controller); }).then(function () {
      self._pulling = false;
      if (self._reads.length || self._hwm - self._queued > 0) self._pullSoon();
    }, function (e) { self._fail(e); });
  };
  ReadableStream.prototype._finish = function () {
    this._state = "closed";
    this._reads.splice(0).forEach(function (r) { r.resolve({ value: undefined, done: true }); });
    this._closedWaits.splice(0).forEach(function (w) { w.resolve(); });
  };
  ReadableStream.prototype._fail = function (e) {
    if (this._state !== "readable") return;
    this._state = "errored";
    this._error = e;
    this._queue = [];
    this._reads.splice(0).forEach(function (r) { r.reject(e); });
    this._closedWaits.splice(0).forEach(function (w) { w.reject(e); });
  };
  ReadableStream.prototype._read = function () {
    var self = this;
    if (this._queue.length) {
      var chunk = this._queue.shift();
      this._queued -= this._size(chunk);
      if (this._closeRequested && !this._queue.length) this._finish();
      else this._pullSoon();
      return resolved({ value: chunk, done: false });
    }
    if (this._state === "closed") return resolved({ value: undefined, done: true });
    if (this._state === "errored") return Promise.reject(this._error);
    return new Promise(function (resolve, reject) {
      self._reads.push({ resolve: resolve, reject: reject });
      self._pullSoon();
    });
  };
  ReadableStream.prototype._cancel = function (reason) {
    if (this._state === "closed") return resolved();
    if (this._state === "errored") return Promise.reject(this._error);
    this._queue = [];
    this._finish();
    var src = this._source;
    return resolved(typeof src.cancel === "function" ? src.cancel(reason) : undefined).then(function () {});
  };
  Object.defineProperty(ReadableStream.prototype, "locked", { get: function () { return !!this._reader; }, configurable: true });
  ReadableStream.prototype.getReader = function (opts) {
    if (this._reader) throw new TypeError("ReadableStreamDefaultReader constructor can only accept readable streams that are not yet locked to a reader");
    if (opts && opts.mode === "byob") throw new TypeError("BYOB readers are not supported");
    return new ReadableStreamDefaultReader(this);
  };
  ReadableStream.prototype.cancel = function (reason) {
    if (this._reader) return Promise.reject(new TypeError("Cannot cancel a locked stream"));
    return this._cancel(reason);
  };
  ReadableStream.prototype.pipeTo = function (dest, opts) {
    var reader = this.getReader(), writer = dest.getWriter();
    opts = opts || {};
    var pump = function () {
      return reader.read().then(function (r) {
        if (r.done) {
          reader.releaseLock();
          if (opts.preventClose) { writer.releaseLock(); return; }
          return writer.close();
        }
        return writer.write(r.value).then(pump);
      }, function (e) {
        reader.releaseLock();
        if (!opts.preventAbort) writer.abort(e);
        throw e;
      });
    };
    return pump();
  };
  ReadableStream.prototype.pipeThrough = function (pair, opts) {
    this.pipeTo(pair.writable, opts).catch(function () {});
    return pair.readable;
  };
  ReadableStream.prototype.tee = function () {
    var reader = this.getReader(), controllers = [], pulling = null;
    var pull = function () {
      if (pulling) return pulling;
      pulling = reader.read().then(function (r) {
        pulling = null;
        controllers.forEach(function (c) { if (r.done) { try { c.close(); } catch (e) {} } else { try { c.enqueue(r.value); } catch (e) {} } });
      }, function (e) { controllers.forEach(function (c) { c.error(e); }); });
      return pulling;
    };
    var make = function () { return new ReadableStream({ start: function (c) { controllers.push(c); }, pull: pull }); };
    return [make(), make()];
  };
  ReadableStream.prototype.values = function (opts) {
    var reader = this.getReader(), keep = opts && opts.preventCancel;
    var it = {
      next: function () { return reader.read().then(function (r) { if (r.done) reader.releaseLock(); return r; }); },
      return: function (v) { if (!keep) reader.cancel(); reader.releaseLock(); return resolved({ value: v, done: true }); }
    };
    it[Symbol.asyncIterator] = function () { return this; };
    return it;
  };
  ReadableStream.prototype[Symbol.asyncIterator] = ReadableStream.prototype.values;
  ReadableStream.from = function (items) {
    var list = Array.from(items), i = 0;
    return new ReadableStream({ pull: function (c) { if (i < list.length) c.enqueue(list[i++]); else c.close(); } });
  };

  function ReadableStreamDefaultReader(stream) {
    if (stream._reader) throw new TypeError("This stream is locked to a reader already");
    stream._reader = this;
    this._s = stream;
    var self = this;
    this.closed = new Promise(function (resolve, reject) {
      if (stream._state === "closed") resolve();
      else if (stream._state === "errored") reject(stream._error);
      else stream._closedWaits.push({ resolve: resolve, reject: reject });
    });
    this.closed.catch(function () {});
    void self;
  }
  ReadableStreamDefaultReader.prototype.read = function () {
    if (!this._s) return Promise.reject(new TypeError("This reader has been released"));
    return this._s._read();
  };
  ReadableStreamDefaultReader.prototype.cancel = function (reason) { return this._s ? this._s._cancel(reason) : resolved(); };
  ReadableStreamDefaultReader.prototype.releaseLock = function () {
    if (!this._s) return;
    var s = this._s;
    s._reads.splice(0).forEach(function (r) { r.reject(new TypeError("The reader was released")); });
    s._reader = null;
    this._s = null;
  };

  function WritableStreamDefaultController(stream) { this._s = stream; this.signal = typeof AbortController === "function" ? new AbortController().signal : undefined; }
  WritableStreamDefaultController.prototype.error = function (e) { this._s._fail(e); };

  function WritableStream(sink, strategy) {
    if (!(this instanceof WritableStream)) throw new TypeError("Failed to construct 'WritableStream': Please use the 'new' operator");
    sink = sink || {};
    var self = this;
    this._sink = sink;
    this._state = "writable";
    this._writer = null;
    this._hwm = sizeOf(strategy).hwm;
    this._pending = 0;
    this._controller = new WritableStreamDefaultController(this);
    this._chain = resolved(typeof sink.start === "function" ? sink.start(this._controller) : undefined);
    this._chain.catch(function (e) { self._fail(e); });
  }
  WritableStream.prototype._fail = function (e) {
    if (this._state === "errored") return;
    this._state = "errored";
    this._error = e;
  };
  WritableStream.prototype._then = function (step) {
    var self = this;
    this._pending++;
    var p = this._chain.then(function () {
      if (self._state === "errored") throw self._error;
      return step();
    });
    this._chain = p.then(function () { self._pending--; }, function (e) { self._pending--; self._fail(e); });
    return p;
  };
  Object.defineProperty(WritableStream.prototype, "locked", { get: function () { return !!this._writer; }, configurable: true });
  WritableStream.prototype.getWriter = function () {
    if (this._writer) throw new TypeError("This stream is locked to a writer already");
    return new WritableStreamDefaultWriter(this);
  };
  WritableStream.prototype.close = function () {
    if (this._writer) return Promise.reject(new TypeError("Cannot close a locked stream"));
    return this._close();
  };
  WritableStream.prototype._close = function () {
    var self = this, sink = this._sink;
    return this._then(function () {
      self._state = "closed";
      return typeof sink.close === "function" ? sink.close() : undefined;
    });
  };
  WritableStream.prototype.abort = function (reason) {
    var sink = this._sink;
    this._fail(reason);
    return resolved(typeof sink.abort === "function" ? sink.abort(reason) : undefined).then(function () {});
  };

  function WritableStreamDefaultWriter(stream) {
    stream._writer = this;
    this._s = stream;
    var self = this;
    this.ready = resolved();
    this.closed = new Promise(function (resolve, reject) { self._closedResolve = resolve; self._closedReject = reject; });
    this.closed.catch(function () {});
  }
  Object.defineProperty(WritableStreamDefaultWriter.prototype, "desiredSize", {
    get: function () { return this._s ? this._s._hwm - this._s._pending : null; }, configurable: true
  });
  WritableStreamDefaultWriter.prototype.write = function (chunk) {
    var s = this._s, sink = s._sink, c = s._controller;
    if (s._state !== "writable") return Promise.reject(s._error || new TypeError("The stream is closed"));
    return s._then(function () { return typeof sink.write === "function" ? sink.write(chunk, c) : undefined; });
  };
  WritableStreamDefaultWriter.prototype.close = function () {
    var self = this;
    return this._s._close().then(function () { self._closedResolve(); }, function (e) { self._closedReject(e); throw e; });
  };
  WritableStreamDefaultWriter.prototype.abort = function (reason) { return this._s.abort(reason); };
  WritableStreamDefaultWriter.prototype.releaseLock = function () { if (this._s) { this._s._writer = null; this._s = null; } };

  function TransformStream(transformer, writableStrategy, readableStrategy) {
    if (!(this instanceof TransformStream)) throw new TypeError("Failed to construct 'TransformStream': Please use the 'new' operator");
    transformer = transformer || {};
    var rc, started;
    var readable = new ReadableStream({ start: function (c) { rc = c; } }, readableStrategy);
    var controller = {
      enqueue: function (chunk) { rc.enqueue(chunk); },
      error: function (e) { rc.error(e); },
      terminate: function () { try { rc.close(); } catch (e) {} },
      get desiredSize() { return rc.desiredSize; }
    };
    started = resolved(typeof transformer.start === "function" ? transformer.start(controller) : undefined);
    var writable = new WritableStream({
      write: function (chunk) {
        return started.then(function () {
          if (typeof transformer.transform === "function") return transformer.transform(chunk, controller);
          controller.enqueue(chunk);
        });
      },
      close: function () {
        return started.then(function () { return typeof transformer.flush === "function" ? transformer.flush(controller) : undefined; })
          .then(function () { controller.terminate(); });
      },
      abort: function (reason) { rc.error(reason); }
    }, writableStrategy);
    this.readable = readable;
    this.writable = writable;
  }

  define(win, "ReadableStream", ReadableStream);
  define(win, "ReadableStreamDefaultReader", ReadableStreamDefaultReader);
  define(win, "ReadableStreamDefaultController", ReadableStreamDefaultController);
  define(win, "WritableStream", WritableStream);
  define(win, "WritableStreamDefaultWriter", WritableStreamDefaultWriter);
  define(win, "WritableStreamDefaultController", WritableStreamDefaultController);
  define(win, "TransformStream", TransformStream);
  define(win, "ByteLengthQueuingStrategy", function ByteLengthQueuingStrategy(o) { this.highWaterMark = o.highWaterMark; this.size = function (c) { return c.byteLength; }; });
  define(win, "CountQueuingStrategy", function CountQueuingStrategy(o) { this.highWaterMark = o.highWaterMark; this.size = function () { return 1; }; });

  // the whole input, then (de)compressed at the end (by the browser's own code)
  var zipStream = function (name, mode) {
    var Ctor = function (format) {
      if (!(this instanceof Ctor)) throw new TypeError("Failed to construct '" + name + "': Please use the 'new' operator");
      format = String(format);
      if (format !== "gzip" && format !== "deflate" && format !== "deflate-raw")
        throw new TypeError("Failed to construct '" + name + "': Unsupported compression format: '" + format + "'");
      var parts = "";
      var t = new TransformStream({
        transform: function (chunk) {
          if (!(chunk instanceof ArrayBuffer || ArrayBuffer.isView(chunk))) throw new TypeError("The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
          parts += rawBytes(chunk);
        },
        flush: function (c) {
          var out = zip(parts, format, mode);
          if (out === null) throw new TypeError("The compressed data was not valid");
          c.enqueue(new Uint8Array(rawBuffer(out)));
        }
      });
      this.readable = t.readable;
      this.writable = t.writable;
    };
    return Ctor;
  };
  if (zip) {
    define(win, "CompressionStream", zipStream("CompressionStream", 0));
    define(win, "DecompressionStream", zipStream("DecompressionStream", 1));
  }
  define(win, "TextEncoderStream", function TextEncoderStream() {
    var e = new TextEncoder(), t = new TransformStream({ transform: function (s, c) { c.enqueue(e.encode(String(s))); } });
    this.encoding = "utf-8";
    this.readable = t.readable;
    this.writable = t.writable;
  });
  define(win, "TextDecoderStream", function TextDecoderStream() {
    var d = new TextDecoder(), t = new TransformStream({ transform: function (b, c) { c.enqueue(d.decode(b)); } });
    this.encoding = "utf-8";
    this.readable = t.readable;
    this.writable = t.writable;
  });
  if (typeof Blob === "function") {
    Blob.prototype.stream = function () {
      var bytes = new Uint8Array(rawBuffer(this._text)), sent = false;
      return new ReadableStream({ pull: function (c) { if (!sent) { sent = true; c.enqueue(bytes); } else c.close(); } });
    };
  }
})();
