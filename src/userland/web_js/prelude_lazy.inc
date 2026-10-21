// The platform objects (Intl, Streams, crypto.subtle, navigator extras, ...) web_js.cpp installs lazily: kLazyStub registers
// accessors for their names and evaluates kLazyChunk the first time any is touched. Readable sources: edit in place here.
const char kLazyStub[] = R"JS(// Platform objects a page may probe for (Intl, Streams, crypto.subtle, navigator.clipboard, ...). They live in one
// separate script, evaluated the first time any of them is touched: parsing it costs ~18 ms, and most pages never ask.
(function (window) {
  'use strict';
  var fetchSource = window.__aiosLazySource;
  Object.defineProperty(window, '__aiosLazySource', { value: undefined, configurable: true, writable: true });
  delete window.__aiosLazySource;
  var IterProto = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
  var NavProto = Object.getPrototypeOf(window.navigator) || window.navigator;
  var CryptoTarget = window.crypto ? (Object.getPrototypeOf(window.crypto) === Object.prototype ? window.crypto : Object.getPrototypeOf(window.crypto)) : null;
  var sites = [];
  function add(target, names) { names.forEach(function (n) { if (target && !(n in target)) sites.push([target, n]); }); }
  add(window, ['Intl', 'DOMException', 'DOMRect', 'DOMRectReadOnly', 'DOMPoint', 'DOMPointReadOnly', 'DOMMatrix', 'DOMMatrixReadOnly', 'reportError', 'origin', 'crossOriginIsolated',
    'PromiseRejectionEvent', 'TouchEvent', 'DragEvent', 'ClipboardEvent', 'BeforeUnloadEvent', 'HashChangeEvent', 'PageTransitionEvent', 'TrackEvent', 'SecurityPolicyViolationEvent', 'CloseEvent',
    'visualViewport', 'VisualViewport', 'trustedTypes', 'Notification', 'PerformanceObserver', 'BroadcastChannel', 'WebSocket', 'EventSource',
    'ReadableStream', 'ReadableStreamDefaultReader', 'WritableStream', 'WritableStreamDefaultWriter', 'TransformStream', 'TextDecoderStream', 'TextEncoderStream', 'CountQueuingStrategy', 'ByteLengthQueuingStrategy',
    'SubtleCrypto', 'CryptoKey', 'indexedDB', 'IDBRequest', 'IDBOpenDBRequest', 'IDBKeyRange', 'IDBFactory', 'IDBDatabase', 'IDBTransaction', 'IDBObjectStore', 'caches', 'Atomics',
    'Iterator', 'FileReader', 'DOMParser', 'XMLSerializer', 'NodeFilter', 'TreeWalker', 'NodeIterator', 'FontFace']);
  add(NavProto, ['clipboard', 'permissions', 'serviceWorker', 'mediaDevices', 'connection', 'storage', 'userAgentData', 'locks', 'credentials', 'webdriver', 'doNotTrack', 'pdfViewerEnabled', 'deviceMemory', 'getBattery']);
  add(CryptoTarget, ['subtle']);
  add(Object.getPrototypeOf(window.document), ['implementation']);
  add(window, ['DOMImplementation']);
  add(IterProto, ['map', 'filter', 'take', 'drop', 'flatMap', 'reduce', 'toArray', 'forEach', 'some', 'every', 'find']);
  add(window.Blob && window.Blob.prototype, ['stream']);
  add(Array, ['fromAsync']); add(Promise, ['try']);
  var loaded = false;
  function load() {
    if (loaded) return; loaded = true;
    sites.forEach(function (s) { delete s[0][s[1]]; });
    (0, eval)(fetchSource());
  }
  sites.forEach(function (s) {
    Object.defineProperty(s[0], s[1], {
      get: function () { load(); return this[s[1]]; },
      set: function (v) { Object.defineProperty(this, s[1], { value: v, writable: true, configurable: true, enumerable: false }); },
      configurable: true, enumerable: false });
  });
  // `x.toLocaleString(locale, options)` and `a.localeCompare(b, locale, options)` need Intl; the bare calls keep the engine's own.
  var nTLS = Number.prototype.toLocaleString, bTLS = typeof BigInt === 'function' ? BigInt.prototype.toLocaleString : null, nLC = String.prototype.localeCompare;
  Object.defineProperty(Number.prototype, 'toLocaleString', { value: function toLocaleString(l, o) { var v = Number.prototype.valueOf.call(this); return l === undefined && o === undefined ? nTLS.call(v) : new window.Intl.NumberFormat(l, o).format(v); }, writable: true, configurable: true });
  if (bTLS) Object.defineProperty(BigInt.prototype, 'toLocaleString', { value: function toLocaleString(l, o) { var v = BigInt.prototype.valueOf.call(this); return l === undefined && o === undefined ? bTLS.call(v) : new window.Intl.NumberFormat(l, o).format(v); }, writable: true, configurable: true });
  Object.defineProperty(String.prototype, 'localeCompare', { value: function localeCompare(that, l, o) { if (l === undefined && o === undefined) return nLC.call(this, that); return new window.Intl.Collator(l, o).compare(String(this), String(that)); }, writable: true, configurable: true });
  if (typeof Symbol.dispose === 'undefined') { try { Object.defineProperty(Symbol, 'dispose', { value: Symbol('Symbol.dispose') }); Object.defineProperty(Symbol, 'asyncDispose', { value: Symbol('Symbol.asyncDispose') }); } catch (e) { } }
  // <dialog>
  if (typeof window.HTMLDialogElement === 'function') {
    var DP = window.HTMLDialogElement.prototype;
    if (!DP.showModal) {
      Object.defineProperty(DP, 'returnValue', { get: function () { return this._rv === undefined ? '' : this._rv; }, set: function (v) { this._rv = String(v); }, configurable: true });
      DP.show = function () { this.setAttribute('open', ''); }; DP.showModal = function () { this.setAttribute('open', ''); };
      DP.close = function (rv) { if (!this.hasAttribute('open')) return; if (rv !== undefined) this._rv = String(rv); this.removeAttribute('open'); this.dispatchEvent(new Event('close')); };
      DP.requestClose = DP.close;
    }
  }
})(globalThis);
)JS";

const char kLazyChunk[] = R"JS((function (window) {
  'use strict';
  function def(name, value) { Object.defineProperty(window, name, { value: value, writable: true, configurable: true, enumerable: false }); }
  // ---- DOMException, geometry, small platform objects ------------------------------------------------------
  (function () {
    var CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5, NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InUseAttributeError: 10, InvalidStateError: 11, SyntaxError: 12, InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15, TypeMismatchError: 17, SecurityError: 18, NetworkError: 19, AbortError: 20, URLMismatchError: 21, QuotaExceededError: 22, TimeoutError: 23, InvalidNodeTypeError: 24, DataCloneError: 25 };
    if (typeof window.DOMException !== 'function') {
      var DOMException = function DOMException(message, name) {
        if (!(this instanceof DOMException)) throw new TypeError("Failed to construct 'DOMException': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
        var n = name === undefined ? 'Error' : String(name);
        Object.defineProperty(this, '_n', { value: n });
        Object.defineProperty(this, '_m', { value: message === undefined ? '' : String(message) });
        var st = new Error(this._m).stack; if (st) Object.defineProperty(this, 'stack', { value: n + ': ' + this._m + st.replace(/^[^\n]*/, ''), configurable: true, writable: true });
      };
      DOMException.prototype = Object.create(Error.prototype, { constructor: { value: DOMException, writable: true, configurable: true } });
      Object.defineProperty(DOMException.prototype, 'name', { get: function () { return this._n; }, configurable: true });
      Object.defineProperty(DOMException.prototype, 'message', { get: function () { return this._m; }, configurable: true });
      Object.defineProperty(DOMException.prototype, 'code', { get: function () { return CODES[this._n] || 0; }, configurable: true });
      Object.defineProperty(DOMException.prototype, Symbol.toStringTag, { value: 'DOMException', configurable: true });
      Object.keys(CODES).forEach(function (k) {
        var c = k.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase().replace(/_ERROR$/, '_ERR');
        Object.defineProperty(DOMException, c, { value: CODES[k], enumerable: true });
        Object.defineProperty(DOMException.prototype, c, { value: CODES[k], enumerable: true });
      });
      def('DOMException', DOMException);
    }

    function RO(x, y, w, h) { this._x = +x || 0; this._y = +y || 0; this._w = +w || 0; this._h = +h || 0; }
    function prop(P, n, f, set) { Object.defineProperty(P, n, { get: f, set: set, enumerable: true, configurable: true }); }
    ['x', 'y', 'width', 'height'].forEach(function (n, i) { var k = ['_x', '_y', '_w', '_h'][i]; prop(RO.prototype, n, function () { return this[k]; }); });
    prop(RO.prototype, 'top', function () { return Math.min(this._y, this._y + this._h); });
    prop(RO.prototype, 'bottom', function () { return Math.max(this._y, this._y + this._h); });
    prop(RO.prototype, 'left', function () { return Math.min(this._x, this._x + this._w); });
    prop(RO.prototype, 'right', function () { return Math.max(this._x, this._x + this._w); });
    RO.prototype.toJSON = function () { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, right: this.right, bottom: this.bottom, left: this.left }; };
    RO.fromRect = function (r) { r = r || {}; return new RO(r.x, r.y, r.width, r.height); };
    function DOMRect(x, y, w, h) { RO.call(this, x, y, w, h); }
    DOMRect.prototype = Object.create(RO.prototype, { constructor: { value: DOMRect, writable: true, configurable: true } });
    ['x', 'y', 'width', 'height'].forEach(function (n, i) { var k = ['_x', '_y', '_w', '_h'][i]; prop(DOMRect.prototype, n, function () { return this[k]; }, function (v) { this[k] = +v || 0; }); });
    DOMRect.fromRect = function (r) { r = r || {}; return new DOMRect(r.x, r.y, r.width, r.height); };
    if (typeof window.DOMRectReadOnly !== 'function') def('DOMRectReadOnly', RO);
    if (typeof window.DOMRect !== 'function') def('DOMRect', DOMRect);
    function PT(x, y, z, w) { this.x = +x || 0; this.y = +y || 0; this.z = +z || 0; this.w = w === undefined ? 1 : +w; }
    PT.fromPoint = function (p) { p = p || {}; return new PT(p.x, p.y, p.z, p.w); };
    if (typeof window.DOMPoint !== 'function') { def('DOMPoint', PT); def('DOMPointReadOnly', PT); }
    function Matrix(init) {
      var a = Array.isArray(init) ? init : [1, 0, 0, 1, 0, 0];
      this.a = a[0]; this.b = a[1]; this.c = a[2]; this.d = a[3]; this.e = a[4]; this.f = a[5];
    }
    Matrix.prototype.multiply = function (o) { return new Matrix([this.a * o.a + this.c * o.b, this.b * o.a + this.d * o.b, this.a * o.c + this.c * o.d, this.b * o.c + this.d * o.d, this.a * o.e + this.c * o.f + this.e, this.b * o.e + this.d * o.f + this.f]); };
    Matrix.prototype.translate = function (x, y) { return this.multiply(new Matrix([1, 0, 0, 1, x || 0, y || 0])); };
    Matrix.prototype.scale = function (x, y) { return this.multiply(new Matrix([x === undefined ? 1 : x, 0, 0, y === undefined ? (x === undefined ? 1 : x) : y, 0, 0])); };
    Matrix.prototype.inverse = function () { var d = this.a * this.d - this.b * this.c; return new Matrix([this.d / d, -this.b / d, -this.c / d, this.a / d, (this.c * this.f - this.d * this.e) / d, (this.b * this.e - this.a * this.f) / d]); };
    Matrix.prototype.transformPoint = function (p) { return new PT(this.a * p.x + this.c * p.y + this.e, this.b * p.x + this.d * p.y + this.f); };
    Object.defineProperty(Matrix.prototype, 'isIdentity', { get: function () { return this.a === 1 && this.b === 0 && this.c === 0 && this.d === 1 && this.e === 0 && this.f === 0; } });
    Object.defineProperty(Matrix.prototype, 'is2D', { value: true });
    if (typeof window.DOMMatrix !== 'function') { def('DOMMatrix', Matrix); def('DOMMatrixReadOnly', Matrix); }

    // Chrome has these; a page that feature-detects them then calls them must find something that answers.
    def('reportError', function reportError(e) {
      var ev = new ErrorEvent('error', { error: e, message: e && e.message ? String(e.message) : String(e), cancelable: true });
      window.dispatchEvent(ev);
    });
    try { Object.defineProperty(window, 'origin', { get: function () { return location.origin; }, configurable: true, enumerable: true }); } catch (e) { }
    if (!('crossOriginIsolated' in window)) Object.defineProperty(window, 'crossOriginIsolated', { value: false, configurable: true });
    // Event subclassing through the engine's native Event needs a real subclass, so use class syntax.
    function evSub(name, extra) {
      if (typeof window[name] === 'function') return;
      var K = (function (Base) { return class extends Base { constructor(type, init) { super(type, init); init = init || {}; for (var k in extra) Object.defineProperty(this, k, { value: init[k] === undefined ? extra[k] : init[k], enumerable: true }); } }; })(Event);
      Object.defineProperty(K, 'name', { value: name });
      def(name, K);
    }
    evSub('PromiseRejectionEvent', { promise: undefined, reason: undefined });
    evSub('TouchEvent', { touches: [], targetTouches: [], changedTouches: [] });
    evSub('DragEvent', { dataTransfer: null });
    evSub('ClipboardEvent', { clipboardData: null });
    evSub('BeforeUnloadEvent', {});
    evSub('HashChangeEvent', { oldURL: '', newURL: '' });
    evSub('PageTransitionEvent', { persisted: false });
    evSub('TrackEvent', { track: null });
    evSub('SecurityPolicyViolationEvent', { blockedURI: '', violatedDirective: '' });
    evSub('CloseEvent', { code: 0, reason: '', wasClean: false });

    if (!window.visualViewport) {
      var vv = new EventTarget();
      ['width:innerWidth', 'height:innerHeight'].forEach(function (s) { var p = s.split(':'); Object.defineProperty(vv, p[0], { get: function () { return window[p[1]]; }, enumerable: true }); });
      ['offsetLeft', 'offsetTop', 'pageLeft', 'pageTop'].forEach(function (k) { Object.defineProperty(vv, k, { get: function () { return k.indexOf('page') === 0 ? (k === 'pageLeft' ? window.scrollX : window.scrollY) : 0; }, enumerable: true }); });
      vv.scale = 1; vv.onresize = null; vv.onscroll = null;
      def('visualViewport', vv);
      def('VisualViewport', function VisualViewport() { throw new TypeError('Illegal constructor'); });
    }
    if (!window.trustedTypes) {
      var pol = function (name, rules) {
        rules = rules || {};
        var o = { name: name };
        ['createHTML', 'createScript', 'createScriptURL'].forEach(function (k) { o[k] = function () { return rules[k] ? rules[k].apply(rules, arguments) : String(arguments[0]); }; });
        return o;
      };
      def('trustedTypes', { createPolicy: pol, isHTML: function () { return false; }, isScript: function () { return false; }, isScriptURL: function () { return false; }, getAttributeType: function () { return null; }, getPropertyType: function () { return null; }, emptyHTML: '', emptyScript: '', defaultPolicy: null });
    }
    if (typeof window.Notification !== 'function') {
      var N = function Notification(title) { throw new TypeError("Failed to construct 'Notification': Illegal constructor"); };
      N.permission = 'denied'; N.maxActions = 2;
      N.requestPermission = function (cb) { if (typeof cb === 'function') cb('denied'); return Promise.resolve('denied'); };
      def('Notification', N);
    }
    if (typeof window.PerformanceObserver !== 'function') {
      var PO = function PerformanceObserver(cb) { this._cb = cb; };
      PO.prototype.observe = function () { }; PO.prototype.disconnect = function () { }; PO.prototype.takeRecords = function () { return []; };
      PO.supportedEntryTypes = ['mark', 'measure', 'navigation', 'resource', 'paint'];
      def('PerformanceObserver', PO);
    }
    if (typeof window.BroadcastChannel !== 'function') {
      var chans = {};
      var BC = class BroadcastChannel extends EventTarget {
        constructor(name) { super(); this.name = String(name); this.onmessage = null; this._closed = false; (chans[this.name] = chans[this.name] || []).push(this); }
        postMessage(m) {
          if (this._closed) throw new DOMException("Failed to execute 'postMessage' on 'BroadcastChannel': Channel is closed", 'InvalidStateError');
          var from = this, data = structuredClone(m);
          (chans[this.name] || []).slice().forEach(function (c) { if (c !== from && !c._closed) setTimeout(function () { var ev = new MessageEvent('message', { data: data }); if (typeof c.onmessage === 'function') c.onmessage(ev); c.dispatchEvent(ev); }, 0); });
        }
        close() { this._closed = true; var l = chans[this.name] || []; var i = l.indexOf(this); if (i >= 0) l.splice(i, 1); }
      };
      def('BroadcastChannel', BC);
    }
    // No network sockets, no workers: a page that probes for them must see them fail the way an offline browser would.
    function failingSocket(name, states) {
      if (typeof window[name] === 'function') return;
      var K = class extends EventTarget {
        constructor(url) {
          super(); this.url = String(url); this.readyState = 0; this.bufferedAmount = 0; this.protocol = ''; this.extensions = ''; this.binaryType = 'blob';
          this.onopen = this.onmessage = this.onerror = this.onclose = null;
          var s = this;
          setTimeout(function () {
            s.readyState = states ? 2 : 3;
            var e = new Event('error'); if (typeof s.onerror === 'function') s.onerror(e); s.dispatchEvent(e);
            if (name === 'WebSocket') { var c = new CloseEvent('close', { code: 1006, reason: '', wasClean: false }); if (typeof s.onclose === 'function') s.onclose(c); s.dispatchEvent(c); }
          }, 0);
        }
        send() { if (this.readyState === 0) throw new DOMException("Failed to execute 'send' on 'WebSocket': Still in CONNECTING state.", 'InvalidStateError'); }
        close() { this.readyState = states ? 2 : 3; }
      };
      Object.defineProperty(K, 'name', { value: name });
      ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED'].forEach(function (k, i) { Object.defineProperty(K, k, { value: i, enumerable: true }); Object.defineProperty(K.prototype, k, { value: i, enumerable: true }); });
      def(name, K);
    }
    failingSocket('WebSocket', false);
    failingSocket('EventSource', true);
  })();
  // ---- WHATWG Streams (default controllers only; no byte streams, no BYOB) ----------------------------------
  (function () {
    if (typeof window.ReadableStream === 'function' && typeof window.WritableStream === 'function' && typeof window.TransformStream === 'function') return;
    var kState = Symbol('state');
    function Deferred() { var d = {}; d.promise = new Promise(function (a, b) { d.resolve = a; d.reject = b; }); return d; }
    function invalid(m) { return new TypeError(m); }

    function ReadableStream(src, strategy) {
      src = src || {};
      var s = this[kState] = { queue: [], chunks: 0, closeRequested: false, state: 'readable', stored: undefined, reader: null, pulling: false, pullAgain: false, started: false, src: src, hwm: strategy && strategy.highWaterMark !== undefined ? +strategy.highWaterMark : 1, size: strategy && strategy.size, reads: [] };
      var self = this;
      var ctl = { get desiredSize() { return s.state === 'errored' ? null : s.state === 'closed' ? 0 : s.hwm - s.chunks; },
        enqueue: function (c) {
          if (s.closeRequested || s.state !== 'readable') throw invalid('Cannot enqueue a chunk into a closed readable stream');
          if (s.reads.length) s.reads.shift().resolve({ value: c, done: false });
          else { var sz = 1; if (s.size) sz = s.size(c); s.queue.push({ c: c, sz: sz }); s.chunks += sz; }
          pull();
        },
        close: function () {
          if (s.closeRequested || s.state !== 'readable') throw invalid('Cannot close a stream that is already closed or errored');
          s.closeRequested = true; if (!s.queue.length) finish();
        },
        error: function (e) { fail(e); } };
      function finish() { s.state = 'closed'; if (s.reader) { s.reads.splice(0).forEach(function (r) { r.resolve({ value: undefined, done: true }); }); s.reader._closed.resolve(); } }
      function fail(e) { if (s.state !== 'readable') return; s.state = 'errored'; s.stored = e; s.queue = []; if (s.reader) { s.reads.splice(0).forEach(function (r) { r.reject(e); }); s.reader._closed.reject(e); s.reader._closed.promise.catch(function () { }); } }
      function pull() {
        if (!s.started || s.state !== 'readable' || s.closeRequested) return;
        if (!(s.reads.length || s.hwm - s.chunks > 0)) return;
        if (s.pulling) { s.pullAgain = true; return; }
        if (typeof src.pull !== 'function') return;
        s.pulling = true;
        Promise.resolve().then(function () { return src.pull(ctl); }).then(function () { s.pulling = false; if (s.pullAgain) { s.pullAgain = false; pull(); } }, function (e) { s.pulling = false; fail(e); });
      }
      s.pull = pull; s.fail = fail; s.finish = finish;
      try {
        var r = typeof src.start === 'function' ? src.start(ctl) : undefined;
        Promise.resolve(r).then(function () { s.started = true; pull(); }, function (e) { s.started = true; fail(e); });
      } catch (e) { fail(e); }
    }
    function Reader(stream) {
      var s = stream[kState];
      if (s.reader) throw invalid('ReadableStreamDefaultReader constructor can only accept readable streams that are not yet locked to a reader');
      s.reader = this; this._s = stream; this._closed = Deferred(); this._closed.promise.catch(function () { });
      if (s.state === 'closed') this._closed.resolve(); else if (s.state === 'errored') this._closed.reject(s.stored);
    }
    Reader.prototype.read = function () {
      var s = this._s && this._s[kState]; if (!s) return Promise.reject(invalid('This readable stream reader has been released and cannot be used to read from its previous owner stream'));
      if (s.queue.length) { var it = s.queue.shift(); s.chunks -= it.sz; var v = it.c; if (s.closeRequested && !s.queue.length) s.finish(); else s.pull(); return Promise.resolve({ value: v, done: false }); }
      if (s.state === 'closed') return Promise.resolve({ value: undefined, done: true });
      if (s.state === 'errored') return Promise.reject(s.stored);
      var d = Deferred(); s.reads.push(d); s.pull(); return d.promise;
    };
    Reader.prototype.releaseLock = function () { var s = this._s && this._s[kState]; if (!s) return; s.reader = null; s.reads.splice(0).forEach(function (r) { r.reject(invalid('Reader released')); }); this._s = null; };
    Reader.prototype.cancel = function (reason) { return this._s ? this._s.cancel(reason) : Promise.reject(invalid('Reader released')); };
    Object.defineProperty(Reader.prototype, 'closed', { get: function () { return this._closed.promise; } });
    ReadableStream.prototype.getReader = function (o) { if (o && o.mode === 'byob') throw new TypeError('BYOB readers are not supported'); return new Reader(this); };
    Object.defineProperty(ReadableStream.prototype, 'locked', { get: function () { return !!this[kState].reader; } });
    ReadableStream.prototype.cancel = function (reason) {
      var s = this[kState]; if (s.reader && this.locked && false) return Promise.reject(invalid('locked'));
      if (s.state === 'closed') return Promise.resolve(); if (s.state === 'errored') return Promise.reject(s.stored);
      s.queue = []; s.chunks = 0; s.finish();
      try { return Promise.resolve(typeof s.src.cancel === 'function' ? s.src.cancel(reason) : undefined).then(function () { }); } catch (e) { return Promise.reject(e); }
    };
    ReadableStream.prototype.tee = function () {
      var r = this.getReader(), reason1, reason2, c1, c2, canceled1 = false, canceled2 = false, reading = false;
      function pull() {
        if (reading) return Promise.resolve(); reading = true;
        return r.read().then(function (x) { reading = false; if (x.done) { if (!canceled1) c1.close(); if (!canceled2) c2.close(); return; } if (!canceled1) c1.enqueue(x.value); if (!canceled2) c2.enqueue(x.value); }, function (e) { reading = false; c1.error(e); c2.error(e); });
      }
      var b1 = new ReadableStream({ start: function (c) { c1 = c; }, pull: pull, cancel: function (rs) { canceled1 = true; reason1 = rs; if (canceled2) return r.cancel([reason1, reason2]); } });
      var b2 = new ReadableStream({ start: function (c) { c2 = c; }, pull: pull, cancel: function (rs) { canceled2 = true; reason2 = rs; if (canceled1) return r.cancel([reason1, reason2]); } });
      return [b1, b2];
    };
    ReadableStream.prototype.pipeTo = function (dest, opts) {
      opts = opts || {};
      var r = this.getReader(), w = dest.getWriter();
      return new Promise(function (resolve, reject) {
        function step() {
          w.ready.then(function () { return r.read(); }).then(function (x) {
            if (x.done) { return (opts.preventClose ? Promise.resolve() : w.close()).then(function () { r.releaseLock(); w.releaseLock(); resolve(); }); }
            return w.write(x.value).then(step);
          }).catch(function (e) { r.releaseLock(); if (!opts.preventAbort) { try { w.abort(e); } catch (_) { } } w.releaseLock(); reject(e); });
        }
        step();
      });
    };
    ReadableStream.prototype.pipeThrough = function (pair, opts) { this.pipeTo(pair.writable, opts).catch(function () { }); return pair.readable; };
    ReadableStream.prototype.values = function () {
      var r = this.getReader();
      return { next: function () { return r.read(); }, return: function (v) { r.releaseLock(); return Promise.resolve({ value: v, done: true }); }, [Symbol.asyncIterator]: function () { return this; } };
    };
    ReadableStream.prototype[Symbol.asyncIterator] = ReadableStream.prototype.values;
    ReadableStream.from = function (it) {
      var iter = it[Symbol.asyncIterator] ? it[Symbol.asyncIterator]() : it[Symbol.iterator]();
      return new ReadableStream({ pull: function (c) { return Promise.resolve(iter.next()).then(function (x) { if (x.done) c.close(); else c.enqueue(x.value); }); }, cancel: function (r) { if (iter.return) return iter.return(r); } });
    };
    Object.defineProperty(ReadableStream.prototype, Symbol.toStringTag, { value: 'ReadableStream', configurable: true });
    Object.defineProperty(ReadableStream.prototype, kState, { writable: true, value: null });

    function WritableStream(sink, strategy) {
      sink = sink || {};
      var s = this[kState] = { sink: sink, queue: Promise.resolve(), state: 'writable', stored: undefined, writer: null, hwm: strategy && strategy.highWaterMark !== undefined ? +strategy.highWaterMark : 1, pending: 0, started: null };
      var self = this;
      var ctl = { error: function (e) { s.state = 'errored'; s.stored = e; }, signal: new AbortController().signal };
      s.ctl = ctl;
      try { s.started = Promise.resolve(typeof sink.start === 'function' ? sink.start(ctl) : undefined); } catch (e) { s.state = 'errored'; s.stored = e; s.started = Promise.resolve(); }
    }
    function Writer(stream) {
      var s = stream[kState];
      if (s.writer) throw invalid('Failed to execute \'getWriter\' on \'WritableStream\': Cannot create a writer when the stream is locked');
      s.writer = this; this._s = stream; this._closed = Deferred(); this._closed.promise.catch(function () { });
    }
    Object.defineProperty(Writer.prototype, 'ready', { get: function () { var s = this._s[kState]; return s.state === 'errored' ? Promise.reject(s.stored) : s.started.then(function () { }); } });
    Object.defineProperty(Writer.prototype, 'closed', { get: function () { return this._closed.promise; } });
    Object.defineProperty(Writer.prototype, 'desiredSize', { get: function () { var s = this._s[kState]; return s.state === 'errored' ? null : s.hwm - s.pending; } });
    Writer.prototype.write = function (c) {
      var st = this._s, s = st[kState], self = this;
      if (s.state === 'errored') return Promise.reject(s.stored);
      if (s.state !== 'writable') return Promise.reject(invalid('Cannot write to a closing or closed stream'));
      s.pending++;
      var p = s.queue = s.queue.then(function () { return s.started; }).then(function () { if (s.state === 'errored') throw s.stored; if (typeof s.sink.write === 'function') return s.sink.write(c, s.ctl); }).then(function () { s.pending--; }, function (e) { s.pending--; s.state = 'errored'; s.stored = e; throw e; });
      p.catch(function () { });
      return p;
    };
    Writer.prototype.close = function () {
      var s = this._s[kState], self = this;
      if (s.state === 'errored') return Promise.reject(s.stored);
      if (s.state !== 'writable') return Promise.reject(invalid('Cannot close a closing or closed stream'));
      s.state = 'closing';
      return s.queue = s.queue.then(function () { if (typeof s.sink.close === 'function') return s.sink.close(); }).then(function () { s.state = 'closed'; self._closed.resolve(); });
    };
    Writer.prototype.abort = function (reason) {
      var s = this._s[kState]; if (s.state === 'closed' || s.state === 'errored') return Promise.resolve();
      s.state = 'errored'; s.stored = reason; this._closed.reject(reason);
      return Promise.resolve(typeof s.sink.abort === 'function' ? s.sink.abort(reason) : undefined).then(function () { });
    };
    Writer.prototype.releaseLock = function () { this._s[kState].writer = null; };
    WritableStream.prototype.getWriter = function () { return new Writer(this); };
    Object.defineProperty(WritableStream.prototype, 'locked', { get: function () { return !!this[kState].writer; } });
    WritableStream.prototype.abort = function (r) { return this.getWriter().abort(r); };
    WritableStream.prototype.close = function () { return this.getWriter().close(); };
    Object.defineProperty(WritableStream.prototype, Symbol.toStringTag, { value: 'WritableStream', configurable: true });

    function TransformStream(tr, ws, rs) {
      tr = tr || {}; var rc, self = this, pending = null;
      var ctl = { enqueue: function (c) { rc.enqueue(c); }, error: function (e) { rc.error(e); }, terminate: function () { rc.close(); }, get desiredSize() { return rc.desiredSize; } };
      this.readable = new ReadableStream({ start: function (c) { rc = c; } }, rs || { highWaterMark: 0 });
      this.writable = new WritableStream({
        start: function () { if (typeof tr.start === 'function') return tr.start(ctl); },
        write: function (c) { return typeof tr.transform === 'function' ? tr.transform(c, ctl) : ctl.enqueue(c); },
        close: function () { return Promise.resolve(typeof tr.flush === 'function' ? tr.flush(ctl) : undefined).then(function () { try { rc.close(); } catch (e) { } }); },
        abort: function (e) { rc.error(e); }
      }, ws || { highWaterMark: 1 });
    }
    Object.defineProperty(TransformStream.prototype, Symbol.toStringTag, { value: 'TransformStream', configurable: true });

    function Strategy(name, f) {
      var K = function (o) { if (!o || o.highWaterMark === undefined) throw new TypeError("Failed to construct '" + name + "': required member highWaterMark is undefined."); this.highWaterMark = +o.highWaterMark; };
      Object.defineProperty(K.prototype, 'size', { value: f, writable: true, configurable: true });
      Object.defineProperty(K, 'name', { value: name }); def(name, K);
    }
    Strategy('CountQueuingStrategy', function () { return 1; });
    Strategy('ByteLengthQueuingStrategy', function (c) { return c.byteLength; });

    function TextDecoderStream(label, opts) {
      var d = new TextDecoder(label, opts);
      var t = new TransformStream({ transform: function (c, k) { var s = d.decode(c, { stream: true }); if (s) k.enqueue(s); }, flush: function (k) { var s = d.decode(); if (s) k.enqueue(s); } });
      this.readable = t.readable; this.writable = t.writable; this.encoding = d.encoding; this.fatal = d.fatal; this.ignoreBOM = d.ignoreBOM;
    }
    function TextEncoderStream() {
      var e = new TextEncoder();
      var t = new TransformStream({ transform: function (c, k) { k.enqueue(e.encode(c)); } });
      this.readable = t.readable; this.writable = t.writable; this.encoding = 'utf-8';
    }
    if (typeof Blob === 'function' && !Blob.prototype.stream) Object.defineProperty(Blob.prototype, 'stream', { value: function stream() { var b = this; return new ReadableStream({ start: function (c) { return b.arrayBuffer().then(function (ab) { if (ab.byteLength) c.enqueue(new Uint8Array(ab)); c.close(); }); } }); }, writable: true, configurable: true });
    def('ReadableStream', ReadableStream); def('ReadableStreamDefaultReader', Reader); def('WritableStream', WritableStream); def('WritableStreamDefaultWriter', Writer);
    def('TransformStream', TransformStream); def('TextDecoderStream', TextDecoderStream); def('TextEncoderStream', TextEncoderStream);
  })();
  // ---- Intl: en-US first, a handful of other locales' separators; dates ride on the engine's own Date.toLocale*String ----
  (function () {
    if (typeof window.Intl === 'object' && window.Intl && typeof Intl.NumberFormat === 'function') return;
    var Intl = {};
    var LANGTAG = /^(?:[a-z]{2,3}(?:-[a-z]{3}){0,3}|[a-z]{5,8})(?:-[a-z]{4})?(?:-(?:[a-z]{2}|\d{3}))?(?:-(?:[a-z0-9]{5,8}|\d[a-z0-9]{3}))*(?:-[0-9a-wy-z](?:-[a-z0-9]{2,8})+)*(?:-x(?:-[a-z0-9]{1,8})+)?$/i;
    function canon(tag) {
      if (typeof tag !== 'string' && !(tag && typeof tag === 'object')) throw new TypeError('Incorrect locale information provided');
      var s = String(tag).replace(/_/g, '-');
      if (!LANGTAG.test(s)) throw new RangeError('Invalid language tag: ' + s);
      var p = s.split('-'), inExt = false;
      p[0] = p[0].toLowerCase();
      for (var i = 1; i < p.length; i++) { if (p[i].length === 1) inExt = true; if (inExt) p[i] = p[i].toLowerCase(); else if (p[i].length === 4 && /^[A-Za-z]+$/.test(p[i])) p[i] = p[i][0].toUpperCase() + p[i].slice(1).toLowerCase(); else if (p[i].length === 2 && /^[A-Za-z]+$/.test(p[i])) p[i] = p[i].toUpperCase(); else p[i] = p[i].toLowerCase(); }
      return p.join('-');
    }
    function locList(l) { if (l === undefined) return []; if (typeof l === 'string' || (l && l.baseName !== undefined && !Array.isArray(l))) return [canon(String(l))]; var out = []; for (var i = 0; i < l.length; i++) { var c = canon(l[i]); if (out.indexOf(c) < 0) out.push(c); } return out; }
    function pickLocale(l) { var list = locList(l); return list.length ? list[0] : 'en-US'; }
    function lang(l) { return l.split('-')[0]; }
    function getOpt(o, k, allowed, dflt) { var v = o[k]; if (v === undefined) return dflt; v = String(v); if (allowed && allowed.indexOf(v) < 0) throw new RangeError('Value ' + v + ' out of range for Intl options property ' + k); return v; }
    function getNum(o, k, lo, hi, dflt) { var v = o[k]; if (v === undefined) return dflt; v = Number(v); if (isNaN(v) || v < lo || v > hi) throw new RangeError(k + ' value is out of range.'); return Math.floor(v); }
    function toObj(o) { return o === undefined ? {} : Object(o); }
    function supported(l) { return locList(l); }
    function mk(name, ctor, extra) {
      ctor.supportedLocalesOf = function (l) { return supported(l); };
      Object.defineProperty(ctor.prototype, Symbol.toStringTag, { value: 'Intl.' + name, configurable: true });
      Object.defineProperty(Intl, name, { value: ctor, writable: true, configurable: true });
    }

    // ---------- NumberFormat ----------
    var SEP = { en: [',', '.'], de: ['.', ','], es: ['.', ','], it: ['.', ','], pt: ['.', ','], nl: ['.', ','], tr: ['.', ','], id: ['.', ','], da: ['.', ','], fr: [' ', ','], ru: [' ', ','], sv: [' ', ','], nb: [' ', ','], fi: [' ', ','], pl: [' ', ','], cs: [' ', ','], uk: [' ', ','] };
    var SUFFIX_CUR = { de: 1, fr: 1, es: 1, it: 1, ru: 1, sv: 1, nb: 1, fi: 1, pl: 1, cs: 1, uk: 1, da: 1, tr: 0 };
    var SYM = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', INR: '₹', CNY: 'CN¥', KRW: '₩', CAD: 'CA$', AUD: 'A$', MXN: 'MX$', BRL: 'R$', NZD: 'NZ$', HKD: 'HK$', ILS: '₪', VND: '₫', TWD: 'NT$', XAF: 'FCFA', PHP: '₱' };
    var CUR_DIGITS = { JPY: 0, KRW: 0, VND: 0, CLP: 0, ISK: 0, UGX: 0, XAF: 0, XOF: 0, BHD: 3, KWD: 3, JOD: 3, OMR: 3, TND: 3 };
    var CUR_NAME = { USD: ['US dollar', 'US dollars'], EUR: ['euro', 'euros'], GBP: ['British pound', 'British pounds'], JPY: ['Japanese yen', 'Japanese yen'], INR: ['Indian rupee', 'Indian rupees'], CAD: ['Canadian dollar', 'Canadian dollars'], AUD: ['Australian dollar', 'Australian dollars'], CNY: ['Chinese yuan', 'Chinese yuan'] };
    var UNITS = { // short, narrow, long singular, long plural
      byte: ['byte', 'B', 'byte', 'bytes'], kilobyte: ['kB', 'kB', 'kilobyte', 'kilobytes'], megabyte: ['MB', 'MB', 'megabyte', 'megabytes'], gigabyte: ['GB', 'GB', 'gigabyte', 'gigabytes'], terabyte: ['TB', 'TB', 'terabyte', 'terabytes'], petabyte: ['PB', 'PB', 'petabyte', 'petabytes'], bit: ['bit', 'bit', 'bit', 'bits'],
      percent: ['%', '%', 'percent', 'percent'], second: ['sec', 's', 'second', 'seconds'], minute: ['min', 'm', 'minute', 'minutes'], hour: ['hr', 'h', 'hour', 'hours'], day: ['day', 'd', 'day', 'days'], week: ['wk', 'w', 'week', 'weeks'], month: ['mth', 'm', 'month', 'months'], year: ['yr', 'y', 'year', 'years'], millisecond: ['ms', 'ms', 'millisecond', 'milliseconds'],
      meter: ['m', 'm', 'meter', 'meters'], kilometer: ['km', 'km', 'kilometer', 'kilometers'], centimeter: ['cm', 'cm', 'centimeter', 'centimeters'], millimeter: ['mm', 'mm', 'millimeter', 'millimeters'], mile: ['mi', 'mi', 'mile', 'miles'], foot: ['ft', '′', 'foot', 'feet'], inch: ['in', '″', 'inch', 'inches'], yard: ['yd', 'yd', 'yard', 'yards'],
      kilogram: ['kg', 'kg', 'kilogram', 'kilograms'], gram: ['g', 'g', 'gram', 'grams'], pound: ['lb', '#', 'pound', 'pounds'], ounce: ['oz', 'oz', 'ounce', 'ounces'], liter: ['L', 'L', 'liter', 'liters'], milliliter: ['mL', 'mL', 'milliliter', 'milliliters'], celsius: ['°C', '°C', 'degree Celsius', 'degrees Celsius'], fahrenheit: ['°F', '°', 'degree Fahrenheit', 'degrees Fahrenheit'], degree: ['deg', '°', 'degree', 'degrees'],
      'kilometer-per-hour': ['km/h', 'km/h', 'kilometer per hour', 'kilometers per hour'], 'mile-per-hour': ['mph', 'mph', 'mile per hour', 'miles per hour'], 'meter-per-second': ['m/s', 'm/s', 'meter per second', 'meters per second'] };
    function digitsStr(x, minFD, maxFD, minSD, maxSD, minID) {
      var s;
      if (minSD !== undefined || maxSD !== undefined) {
        minSD = minSD || 1; maxSD = maxSD || 21;
        if (x === 0) s = '0'; else { s = Number(x.toPrecision(maxSD)).toString(); if (/e/.test(s)) s = Number(s).toFixed(Math.max(0, maxSD - 1 - Math.floor(Math.log10(x)))); }
        if (/e/.test(s)) s = BigInt(Math.round(Number(s))).toString();
        var digs = s.replace('.', '').replace(/^0+/, '').length;
        if (digs < minSD) { if (s.indexOf('.') < 0) s += '.'; s += '0'.repeat(minSD - digs); if (s.slice(-1) === '.') s = s.slice(0, -1); }
      } else {
        if (x >= 1e15) {
          s = String(x); var em = /^(\d)(?:\.(\d+))?e\+(\d+)$/.exec(s);
          if (em) { var dgs = em[1] + (em[2] || ''); s = dgs + '0'.repeat(Number(em[3]) - dgs.length + 1); }
          if (minFD) s += '.' + '0'.repeat(minFD);
        }
        else {
          s = x.toFixed(Math.min(100, maxFD));
          if (s.indexOf('.') >= 0) { var f = s.split('.')[1]; var keep = f.length; while (keep > minFD && f[keep - 1] === '0') keep--; s = s.split('.')[0] + (keep ? '.' + f.slice(0, keep) : ''); }
        }
      }
      var ip = s.split('.')[0], fp = s.split('.')[1] || '';
      if (ip.length < (minID || 1)) ip = '0'.repeat(minID - ip.length) + ip;
      return [ip, fp];
    }
    function group(ip, sep, loc, usegroup) {
      if (usegroup === false || ip.length < 4) return ip;
      var indian = /^(en-IN|hi)/.test(loc);
      if (/^(es|pt-PT|pl)/.test(loc) && ip.length < 5 && usegroup !== 'always') return ip;
      var out = '';
      if (indian) { var last = ip.slice(-3), rest = ip.slice(0, -3); while (rest.length > 2) { out = sep + rest.slice(-2) + out; rest = rest.slice(0, -2); } return (rest ? rest + out : out.slice(sep.length)) + sep + last; }
      for (var i = 0; i < ip.length; i++) { if (i && (ip.length - i) % 3 === 0) out += sep; out += ip[i]; }
      return out;
    }
    function NumberFormat(locales, options) {
      if (!(this instanceof NumberFormat)) return new NumberFormat(locales, options);
      var o = toObj(options), l = pickLocale(locales), r = this._r = { locale: l, numberingSystem: 'latn' };
      var style = r.style = getOpt(o, 'style', ['decimal', 'percent', 'currency', 'unit'], 'decimal');
      var cur = o.currency; if (cur !== undefined) { cur = String(cur); if (!/^[A-Za-z]{3}$/.test(cur)) throw new RangeError('Invalid currency code : ' + cur); cur = cur.toUpperCase(); }
      if (style === 'currency') { if (cur === undefined) throw new TypeError('Currency code is required with currency style.'); r.currency = cur; r.currencyDisplay = getOpt(o, 'currencyDisplay', ['code', 'symbol', 'narrowSymbol', 'name'], 'symbol'); r.currencySign = getOpt(o, 'currencySign', ['standard', 'accounting'], 'standard'); }
      if (style === 'unit') { if (o.unit === undefined) throw new TypeError('Unit is required with unit style.'); r.unit = String(o.unit); r.unitDisplay = getOpt(o, 'unitDisplay', ['short', 'narrow', 'long'], 'short'); }
      var notation = r.notation = getOpt(o, 'notation', ['standard', 'scientific', 'engineering', 'compact'], 'standard');
      var cd = style === 'currency' ? (CUR_DIGITS[cur] !== undefined ? CUR_DIGITS[cur] : 2) : 0;
      r.minimumIntegerDigits = getNum(o, 'minimumIntegerDigits', 1, 21, 1);
      var mnfd = o.minimumFractionDigits, mxfd = o.maximumFractionDigits, mnsd = o.minimumSignificantDigits, mxsd = o.maximumSignificantDigits;
      var dmn = style === 'currency' && notation === 'standard' ? cd : 0, dmx = style === 'currency' && notation === 'standard' ? cd : style === 'percent' ? 0 : 3;
      if (mnsd !== undefined || mxsd !== undefined) { r.minimumSignificantDigits = getNum(o, 'minimumSignificantDigits', 1, 21, 1); r.maximumSignificantDigits = getNum(o, 'maximumSignificantDigits', r.minimumSignificantDigits, 21, 21); }
      else if (mnfd !== undefined || mxfd !== undefined) {
        mnfd = getNum(o, 'minimumFractionDigits', 0, 100, undefined); mxfd = getNum(o, 'maximumFractionDigits', 0, 100, undefined);
        if (mnfd === undefined) mnfd = Math.min(dmn, mxfd); else if (mxfd === undefined) mxfd = Math.max(dmx, mnfd); else if (mnfd > mxfd) throw new RangeError('maximumFractionDigits value is out of range.');
        r.minimumFractionDigits = mnfd; r.maximumFractionDigits = mxfd;
      } else if (notation === 'compact') { r.compactRound = true; } else { r.minimumFractionDigits = dmn; r.maximumFractionDigits = dmx; }
      r.useGrouping = o.useGrouping === undefined ? (notation === 'compact' ? 'min2' : 'auto') : (o.useGrouping === false || o.useGrouping === 'false' ? false : o.useGrouping === true ? 'always' : String(o.useGrouping));
      if (notation === 'compact') r.compactDisplay = getOpt(o, 'compactDisplay', ['short', 'long'], 'short');
      r.signDisplay = getOpt(o, 'signDisplay', ['auto', 'never', 'always', 'exceptZero', 'negative'], 'auto');
      r.roundingMode = 'halfExpand';
    }
    function nfParts(nf, x) {
      var r = nf._r, parts = [], l = r.locale, lg = lang(l);
      var sep = SEP[lg] || SEP.en; if (l === 'de-CH') sep = ['’', '.']; if (l === 'pt-BR') sep = ['.', ','];
      var neg = typeof x === 'bigint' ? x < 0n : (x < 0 || Object.is(x, -0));
      if (typeof x === 'bigint') x = Number(x); else x = Number(x);
      var isNaN_ = x !== x;
      var ax = Math.abs(x);
      if (r.style === 'percent') ax *= 100;
      var body = [];
      if (isNaN_) body.push({ type: 'nan', value: 'NaN' });
      else if (ax === Infinity) body.push({ type: 'infinity', value: '∞' });
      else {
        var suffix = '', exp = 0;
        if (r.notation === 'compact') {
          var d = 0; var t = ax;
          var mag = ax >= 1e3 ? Math.min(4, Math.floor(Math.log10(ax) / 3)) : 0;
          var scaled = ax / Math.pow(10, mag * 3);
          var rd = r.compactRound ? (scaled < 100 ? Number(scaled.toPrecision(2)) : Math.round(scaled)) : null;
          if (rd !== null && rd >= 1000 && mag < 4) { mag++; scaled = ax / Math.pow(10, mag * 3); rd = scaled < 100 ? Number(scaled.toPrecision(2)) : Math.round(scaled); }
          if (mag) suffix = r.compactDisplay === 'long' ? [' thousand', ' million', ' billion', ' trillion'][mag - 1] : ['K', 'M', 'B', 'T'][mag - 1];
          ax = scaled; if (rd !== null) { ax = rd; }
        } else if (r.notation === 'scientific' || r.notation === 'engineering') {
          exp = ax === 0 ? 0 : Math.floor(Math.log10(ax)); if (r.notation === 'engineering') exp = Math.floor(exp / 3) * 3; ax = ax / Math.pow(10, exp);
        }
        var dg = r.compactRound ? digitsStr(ax, 0, 6, undefined, undefined, 1) : digitsStr(ax, r.minimumFractionDigits, r.maximumFractionDigits, r.minimumSignificantDigits, r.maximumSignificantDigits, r.minimumIntegerDigits);
        if (r.signDisplay !== 'negative' && /^0*(\.0*)?$/.test(dg[0] + '.' + dg[1])) { if (r.signDisplay === 'auto' || r.signDisplay === 'exceptZero' || r.signDisplay === 'never') { } }
        var ipg = group(dg[0], sep[0], l, r.useGrouping === 'min2' && dg[0].length < 5 ? false : r.useGrouping);
        var gparts = ipg.split(sep[0]);
        gparts.forEach(function (g, i) { if (i) body.push({ type: 'group', value: sep[0] }); body.push({ type: 'integer', value: g }); });
        if (dg[1]) { body.push({ type: 'decimal', value: sep[1] }); body.push({ type: 'fraction', value: dg[1] }); }
        if (r.notation === 'scientific' || r.notation === 'engineering') { body.push({ type: 'exponentSeparator', value: 'E' }); body.push({ type: 'exponentInteger', value: String(exp) }); }
        if (suffix) { if (suffix[0] === ' ') body.push({ type: 'literal', value: ' ' }); body.push({ type: 'compact', value: suffix.trim() }); }
        var zero = dg[0].replace(/0/g, '') === '' && dg[1].replace(/0/g, '') === '';
        if (zero && r.signDisplay !== 'negative' && r.signDisplay === 'auto') neg = neg && false || neg;
        if (zero && (r.signDisplay === 'exceptZero')) neg = false, r._zero = true; else r._zero = false;
        if (zero && r.signDisplay === 'negative') neg = false;
        r._isZero = zero;
      }
      var sign = '';
      if (r.signDisplay === 'never') sign = '';
      else if (neg && !(r._isZero && r.signDisplay === 'exceptZero')) sign = '-';
      else if (r.signDisplay === 'always' || (r.signDisplay === 'exceptZero' && !r._isZero && !isNaN_)) sign = '+';
      var accounting = r.currencySign === 'accounting' && sign === '-';
      if (accounting) parts.push({ type: 'literal', value: '(' }); else if (sign) parts.push({ type: sign === '-' ? 'minusSign' : 'plusSign', value: sign });
      var curText = '', suffixed = SUFFIX_CUR[lg] && r.style === 'currency', pre = r.style === 'currency' && !suffixed;
      if (r.style === 'currency') {
        var cu = r.currency;
        if (r.currencyDisplay === 'code') curText = cu; else if (r.currencyDisplay === 'name') curText = '';
        else curText = r.currencyDisplay === 'narrowSymbol' ? (SYM[cu] || cu).replace(/^[A-Z]+(?=[^A-Z])/, '') : (lg === 'en' && l !== 'en-US' && cu === 'USD' && l !== 'en' ? 'US$' : SYM[cu] || cu);
        if (/^en-(CA)/.test(l) && cu === 'CAD') curText = '$'; if (/^en-AU/.test(l) && cu === 'AUD') curText = '$';
        if (/^en-GB/.test(l) && cu === 'GBP') curText = '£';
        if (/^ja/.test(l) && cu === 'JPY') curText = '￥'; if (/^zh/.test(l) && cu === 'CNY') curText = '¥';
        if (r.currencyDisplay !== 'name') {
          if (pre) { parts.push({ type: 'currency', value: curText }); if (/[A-Za-z]$/.test(curText) || (lg === 'pt' || lg === 'nl')) parts.push({ type: 'literal', value: ' ' }); }
        }
      }
      Array.prototype.push.apply(parts, body);
      if (r.style === 'percent') { if (SUFFIX_CUR[lg] && lg !== 'tr') parts.push({ type: 'literal', value: ' ' }); parts.push({ type: 'percentSign', value: '%' }); }
      if (r.style === 'currency') {
        if (r.currencyDisplay === 'name') { var nm = CUR_NAME[r.currency] || [r.currency, r.currency]; parts.push({ type: 'literal', value: ' ' }); parts.push({ type: 'currency', value: (ax === 1 && !body.some(function (b) { return b.type === 'fraction'; })) ? nm[0] : nm[1] }); }
        else if (suffixed) { parts.push({ type: 'literal', value: ' ' }); parts.push({ type: 'currency', value: curText }); }
      }
      if (r.style === 'unit') {
        var u = UNITS[r.unit]; var plural = !(ax === 1 && !body.some(function (b) { return b.type === 'fraction'; }));
        if (!u) { var segs = String(r.unit).split('-per-'); u = [r.unit, r.unit, r.unit, r.unit]; }
        var idx = r.unitDisplay === 'long' ? (plural ? 3 : 2) : r.unitDisplay === 'narrow' ? 1 : 0;
        var ut = u[idx];
        if (r.unitDisplay === 'short' && (r.unit === 'byte' || r.unit === 'bit')) ut = r.unit === 'byte' ? 'byte' : 'bit';
        if (r.unitDisplay === 'short' && /^(day|week|month|year)$/.test(r.unit) && plural) ut = r.unit === 'day' ? 'days' : r.unit === 'week' ? 'wks' : r.unit === 'month' ? 'mths' : 'yrs';
        var tight = r.unitDisplay === 'narrow' || r.unit === 'percent' || /^(celsius|fahrenheit)$/.test(r.unit) && r.unitDisplay !== 'long';
        if (!tight) parts.push({ type: 'literal', value: ' ' });
        parts.push({ type: 'unit', value: ut });
      }
      if (accounting) parts.push({ type: 'literal', value: ')' });
      return parts;
    }
    NumberFormat.prototype.formatToParts = function formatToParts(x) { return nfParts(this, typeof x === 'bigint' ? x : Number(x)); };
    function nfFormat(x) { return nfParts(this, typeof x === 'bigint' ? x : Number(x)).map(function (p) { return p.value; }).join(''); }
    Object.defineProperty(NumberFormat.prototype, 'format', { get: function () { var self = this; if (!this._bound) Object.defineProperty(this, '_bound', { value: function (x) { return nfFormat.call(self, x); } }); return this._bound; }, configurable: true });
    NumberFormat.prototype.formatRange = function (a, b) { return this.format(a) + '–' + this.format(b); };
    NumberFormat.prototype.resolvedOptions = function () {
      var r = this._r, o = { locale: r.locale, numberingSystem: 'latn', style: r.style };
      if (r.style === 'currency') { o.currency = r.currency; o.currencyDisplay = r.currencyDisplay; o.currencySign = r.currencySign; }
      if (r.style === 'unit') { o.unit = r.unit; o.unitDisplay = r.unitDisplay; }
      o.minimumIntegerDigits = r.minimumIntegerDigits;
      if (r.minimumSignificantDigits !== undefined) { o.minimumSignificantDigits = r.minimumSignificantDigits; o.maximumSignificantDigits = r.maximumSignificantDigits; }
      else { o.minimumFractionDigits = r.minimumFractionDigits === undefined ? 0 : r.minimumFractionDigits; o.maximumFractionDigits = r.maximumFractionDigits === undefined ? 0 : r.maximumFractionDigits; }
      o.useGrouping = r.useGrouping === 'min2' ? 'min2' : r.useGrouping; o.notation = r.notation; if (r.compactDisplay) o.compactDisplay = r.compactDisplay;
      o.signDisplay = r.signDisplay; o.roundingMode = 'halfExpand'; o.roundingIncrement = 1; o.roundingPriority = 'auto'; o.trailingZeroDisplay = 'auto';
      return o;
    };
    mk('NumberFormat', NumberFormat);

    // ---------- DateTimeFormat ----------
    var DT_FIELDS = ['weekday', 'era', 'year', 'month', 'day', 'dayPeriod', 'hour', 'minute', 'second', 'fractionalSecondDigits', 'timeZoneName'];
    function DateTimeFormat(locales, options) {
      if (!(this instanceof DateTimeFormat)) return new DateTimeFormat(locales, options);
      var o = toObj(options), r = this._r = { locale: pickLocale(locales), calendar: 'gregory', numberingSystem: 'latn' };
      var opts = {}, any = false;
      DT_FIELDS.forEach(function (k) { if (o[k] !== undefined) { opts[k] = k === 'fractionalSecondDigits' ? Number(o[k]) : String(o[k]); any = true; } });
      ['dateStyle', 'timeStyle'].forEach(function (k) { if (o[k] !== undefined) { opts[k] = getOpt(o, k, ['full', 'long', 'medium', 'short']); any = true; } });
      if ((opts.dateStyle || opts.timeStyle) && DT_FIELDS.some(function (k) { return k !== 'timeZoneName' && o[k] !== undefined; })) throw new TypeError("Can't set option " + DT_FIELDS.filter(function (k) { return o[k] !== undefined; })[0] + ' when ' + (opts.dateStyle ? 'dateStyle' : 'timeStyle') + ' is used');
      if (o.hour12 !== undefined) opts.hour12 = !!o.hour12; if (o.hourCycle !== undefined) opts.hourCycle = String(o.hourCycle);
      var tz = o.timeZone === undefined ? undefined : String(o.timeZone);
      if (tz !== undefined) { if (/^utc$|^gmt$|^etc\/(utc|gmt)$/i.test(tz)) tz = 'UTC'; else if (!/^[A-Za-z_]+(\/[A-Za-z_+\-0-9]+)*$|^[+-]\d\d(:?\d\d)?$/.test(tz)) throw new RangeError('Invalid time zone specified: ' + tz); }
      r.timeZone = tz || 'UTC'; r.explicitTz = tz; r.any = any; r.opts = opts;
      if (!any) { opts.year = 'numeric'; opts.month = 'numeric'; opts.day = 'numeric'; }
      r.hourCycle = opts.hour12 === false || opts.hourCycle === 'h23' ? 'h23' : (opts.hour12 === true || opts.hourCycle === 'h12' || /^en-(US|CA|AU|IN|NZ|PH)|^en$|^hi|^ko|^ar/.test(r.locale) ? 'h12' : 'h23');
    }
    function dtNative(r, d) {
      var t = d instanceof Date ? d : (d === undefined ? new Date() : new Date(typeof d === 'number' || typeof d === 'string' ? d : Number(d)));
      if (isNaN(t.getTime())) throw new RangeError('Invalid time value');
      var o = {}; for (var k in r.opts) o[k] = r.opts[k];
      if (r.explicitTz !== undefined && r.explicitTz !== 'UTC') { if (/^[+-]\d\d/.test(r.explicitTz)) { var m = /^([+-])(\d\d):?(\d\d)?/.exec(r.explicitTz); var off = (m[1] === '-' ? -1 : 1) * (Number(m[2]) * 60 + Number(m[3] || 0)); t = new Date(t.getTime() + off * 60000); o.timeZone = 'UTC'; } else o.timeZone = r.explicitTz; } else o.timeZone = 'UTC';
      if (!('hour12' in o) && r.hourCycle === 'h23' && (o.hour !== undefined || o.timeStyle)) o.hour12 = false;
      return [t, o];
    }
    var NUMDATE = { de: ['d.M.y', 'H'], 'en-GB': ['dd/MM/y', 'H'], 'en-AU': ['dd/MM/y', 'h'], 'en-NZ': ['dd/MM/y', 'h'], 'en-IN': ['d/M/y', 'h'], 'en-CA': ['y-MM-dd', 'h'], fr: ['dd/MM/y', 'H'], es: ['d/M/y', 'H'], it: ['d/M/y', 'H'], pt: ['dd/MM/y', 'H'], nl: ['d-M-y', 'H'], ru: ['dd.MM.y', 'H'], pl: ['d.MM.y', 'H'], tr: ['dd.MM.y', 'H'], sv: ['y-MM-dd', 'H'], ja: ['y/M/d', 'H'], 'zh-CN': ['y/M/d', 'H'], zh: ['y/M/d', 'H'], ko: ['y. M. d.', 'h'] };
    // The engine's own Date.toLocale*String knows en-US only; the default numeric date of the other common locales is a pattern.
    function numericLocale(r, t) {
      var o = r.opts; if (r.any && !(Object.keys(o).every(function (k) { return k === 'year' || k === 'month' || k === 'day' || k === 'hour' || k === 'minute' || k === 'second' || k === 'hour12'; }) && o.year === 'numeric' || !r.any)) { if (r.any) return null; }
      var pat = NUMDATE[r.locale] || NUMDATE[lang(r.locale)]; if (!pat || r.locale === 'en-US') return null;
      if (r.any && !(o.year === 'numeric' && o.month === 'numeric' && o.day === 'numeric' || (o.year === undefined && o.month === undefined && o.day === undefined && (o.hour || o.minute || o.second)))) return null;
      var Y = t.getUTCFullYear(), M = t.getUTCMonth() + 1, D = t.getUTCDate(), p2 = function (n) { return n < 10 ? '0' + n : '' + n; };
      var out = [];
      if (o.year !== undefined || !r.any) out.push(pat[0].replace('y', Y).replace('MM', p2(M)).replace('M', M).replace('dd', p2(D)).replace('d', D));
      if (o.hour !== undefined || o.minute !== undefined || o.second !== undefined) {
        var h = t.getUTCHours(), h12 = pat[1] === 'h' ? (o.hour12 !== false) : o.hour12 === true, parts = [];
        var hh = h12 ? (h % 12 || 12) : h;
        if (o.hour !== undefined) parts.push(h12 || o.hour !== '2-digit' ? String(hh) : p2(hh)); if (o.minute !== undefined) parts.push(p2(t.getUTCMinutes())); if (o.second !== undefined) parts.push(p2(t.getUTCSeconds()));
        if (!h12 && o.hour !== undefined && o.hour === 'numeric') parts[0] = p2(hh);
        out.push(parts.join(':') + (h12 ? ' ' + (h < 12 ? 'am' : 'pm') : ''));
      }
      return out.join(', ');
    }
    DateTimeFormat.prototype.format = undefined;
    Object.defineProperty(DateTimeFormat.prototype, 'format', { get: function () { var self = this; if (!this._bound) Object.defineProperty(this, '_bound', { value: function (d) { var a = dtNative(self._r, d); var nl = numericLocale(self._r, a[0]); if (nl !== null) return nl; try { return a[0].toLocaleString(self._r.locale, a[1]); } catch (e) { delete a[1].timeZone; return a[0].toLocaleString(self._r.locale, a[1]); } } }); return this._bound; }, configurable: true });
    DateTimeFormat.prototype.formatToParts = function (d) {
      var s = this.format(d), a = dtNative(this._r, d), t = a[0], o = a[1], parts = [], i = 0;
      var lit = function (x) { if (x) parts.push({ type: 'literal', value: x }); };
      var comps = [];
      if (o.dateStyle || o.timeStyle) { var ds = { weekday: 'long', month: o.dateStyle === 'full' || o.dateStyle === 'long' ? 'long' : o.dateStyle === 'medium' ? 'short' : 'numeric', day: 'numeric', year: 'numeric' }; if (o.dateStyle !== 'full') delete ds.weekday; comps = o.dateStyle ? ['weekday', 'month', 'day', 'year'].filter(function (k) { return ds[k]; }) : []; if (o.timeStyle) comps = comps.concat(['hour', 'minute', 'second', 'dayPeriod']); }
      else comps = ['weekday', 'month', 'day', 'year', 'hour', 'minute', 'second', 'dayPeriod', 'timeZoneName'].filter(function (k) { return o[k] !== undefined; });
      var found = [];
      comps.forEach(function (k) {
        var oo = { timeZone: o.timeZone }; if (o.hour12 !== undefined) oo.hour12 = o.hour12;
        if (o.dateStyle || o.timeStyle) { oo[k] = k === 'weekday' ? 'long' : k === 'month' ? (o.dateStyle === 'full' || o.dateStyle === 'long' ? 'long' : o.dateStyle === 'medium' ? 'short' : 'numeric') : k === 'dayPeriod' ? undefined : (k === 'hour' || k === 'day' || k === 'year' ? 'numeric' : '2-digit'); }
        else oo[k] = o[k];
        if (k === 'dayPeriod') { oo = { timeZone: o.timeZone, hour: 'numeric', hour12: true }; }
        var txt = t.toLocaleString(this._r.locale, oo); if (k === 'dayPeriod') { var m = /(AM|PM|am|pm|[A-Za-z\.]+)$/.exec(txt); txt = m ? m[1] : txt; }
        else if (k === 'hour' && /\s?[AP]M$/i.test(txt)) txt = txt.replace(/\s?[AP]M$/i, '');
        found.push([k, txt]);
      }, this);
      var pos = 0;
      found.forEach(function (f) { var idx = s.indexOf(f[1], pos); if (idx < 0) return; lit(s.slice(pos, idx)); parts.push({ type: f[0], value: f[1] }); pos = idx + f[1].length; });
      lit(s.slice(pos));
      return parts;
    };
    DateTimeFormat.prototype.formatRange = function (a, b) { return this.format(a) + ' – ' + this.format(b); };
    DateTimeFormat.prototype.resolvedOptions = function () {
      var r = this._r, o = { locale: r.locale, calendar: 'gregory', numberingSystem: 'latn', timeZone: r.timeZone };
      if (r.opts.hour !== undefined || r.opts.timeStyle) { o.hourCycle = r.hourCycle; o.hour12 = r.hourCycle === 'h12'; }
      for (var k in r.opts) if (k !== 'hour12' && k !== 'hourCycle') o[k] = r.opts[k];
      return o;
    };
    mk('DateTimeFormat', DateTimeFormat);

    // ---------- PluralRules ----------
    function PluralRules(locales, options) {
      if (!(this instanceof PluralRules)) throw new TypeError("Constructor Intl.PluralRules requires 'new'");
      var o = toObj(options); this._l = pickLocale(locales); this._t = getOpt(o, 'type', ['cardinal', 'ordinal'], 'cardinal');
      this._nf = new NumberFormat(this._l, { minimumFractionDigits: o.minimumFractionDigits, maximumFractionDigits: o.maximumFractionDigits });
    }
    PluralRules.prototype.select = function (n) {
      n = Number(n); var lg = lang(this._l), a = Math.abs(n), i = Math.floor(a), v = (this._nf._r.minimumFractionDigits || 0) > 0 || a !== i;
      if (this._t === 'ordinal') { if (lg !== 'en') return 'other'; var m10 = i % 10, m100 = i % 100; if (v) return 'other'; if (m10 === 1 && m100 !== 11) return 'one'; if (m10 === 2 && m100 !== 12) return 'two'; if (m10 === 3 && m100 !== 13) return 'few'; return 'other'; }
      if (/^(ja|zh|ko|vi|th|id)$/.test(lg)) return 'other';
      if (lg === 'fr' || lg === 'pt') return (i === 0 || i === 1) ? 'one' : (i !== 0 && i % 1000000 === 0 && !v ? 'many' : 'other');
      if (/^(ru|uk)$/.test(lg)) { if (v) return 'other'; var m10 = i % 10, m100 = i % 100; if (m10 === 1 && m100 !== 11) return 'one'; if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return 'few'; return 'many'; }
      if (lg === 'pl') { if (v) return 'other'; var m10 = i % 10, m100 = i % 100; if (i === 1) return 'one'; if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return 'few'; return 'many'; }
      if (lg === 'ar') { if (v) return 'other'; var m100 = i % 100; if (i === 0) return 'zero'; if (i === 1) return 'one'; if (i === 2) return 'two'; if (m100 >= 3 && m100 <= 10) return 'few'; if (m100 >= 11) return 'many'; return 'other'; }
      return (i === 1 && !v) ? 'one' : 'other';
    };
    PluralRules.prototype.selectRange = function (a, b) { return this.select(b); };
    PluralRules.prototype.resolvedOptions = function () { var lg = lang(this._l); return { locale: this._l, type: this._t, minimumIntegerDigits: 1, minimumFractionDigits: 0, maximumFractionDigits: 3, pluralCategories: this._t === 'ordinal' ? ['few', 'one', 'two', 'other'] : (/^(ja|zh|ko)$/.test(lg) ? ['other'] : ['one', 'other']), roundingMode: 'halfExpand' }; };
    mk('PluralRules', PluralRules);

    // ---------- RelativeTimeFormat ----------
    function RelativeTimeFormat(locales, options) {
      if (!(this instanceof RelativeTimeFormat)) throw new TypeError("Constructor Intl.RelativeTimeFormat requires 'new'");
      var o = toObj(options); this._l = pickLocale(locales); this._n = getOpt(o, 'numeric', ['always', 'auto'], 'always'); this._s = getOpt(o, 'style', ['long', 'short', 'narrow'], 'long');
    }
    var RT_UNITS = ['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'];
    var RT_SHORT = { second: 'sec.', minute: 'min.', hour: 'hr.', day: 'day', week: 'wk.', month: 'mo.', quarter: 'qtr.', year: 'yr.' };
    RelativeTimeFormat.prototype.format = function (v, unit) {
      v = Number(v); unit = String(unit).replace(/s$/, ''); if (RT_UNITS.indexOf(unit) < 0) throw new RangeError('Invalid unit argument for format() \'' + unit + '\''); if (!isFinite(v)) throw new RangeError('Invalid time value');
      if (this._n === 'auto') {
        var spec = { day: { '-1': 'yesterday', '0': 'today', '1': 'tomorrow' }, year: { '-1': 'last year', '0': 'this year', '1': 'next year' }, month: { '-1': 'last month', '0': 'this month', '1': 'next month' }, week: { '-1': 'last week', '0': 'this week', '1': 'next week' }, quarter: { '-1': 'last quarter', '0': 'this quarter', '1': 'next quarter' }, hour: { '0': 'this hour' }, minute: { '0': 'this minute' }, second: { '0': 'now' } };
        var w = spec[unit] && spec[unit][String(Object.is(v, -0) ? 0 : v)]; if (w) return w;
      }
      var neg = v < 0 || Object.is(v, -0), a = Math.abs(v), n = new NumberFormat(this._l).format(a);
      var name = this._s === 'long' ? unit + (a === 1 ? '' : 's') : (this._s === 'short' || this._s === 'narrow' ? RT_SHORT[unit] + (unit === 'day' && a !== 1 ? 's' : '') : unit);
      if (this._s === 'narrow' && lang(this._l) === 'en') { var nm = { second: 's', minute: 'm', hour: 'h', day: 'd', week: 'w', month: 'mo', quarter: 'q', year: 'y' }[unit]; return neg ? n + nm + ' ago' : 'in ' + n + nm; }
      return neg ? n + ' ' + name + ' ago' : 'in ' + n + ' ' + name;
    };
    RelativeTimeFormat.prototype.formatToParts = function (v, unit) { var s = this.format(v, unit); return [{ type: 'literal', value: s }]; };
    RelativeTimeFormat.prototype.resolvedOptions = function () { return { locale: this._l, style: this._s, numeric: this._n, numberingSystem: 'latn' }; };
    mk('RelativeTimeFormat', RelativeTimeFormat);

    // ---------- Collator ----------
    function stripAccents(s) { return s.normalize('NFD').replace(/[̀-ͯ]/g, ''); }
    function Collator(locales, options) {
      if (!(this instanceof Collator)) return new Collator(locales, options);
      var o = toObj(options); this._l = pickLocale(locales); this._u = getOpt(o, 'usage', ['sort', 'search'], 'sort'); this._s = getOpt(o, 'sensitivity', ['base', 'accent', 'case', 'variant'], 'variant');
      this._num = o.numeric === undefined ? /-u-.*kn(-true)?/.test(String(locales)) : !!o.numeric; this._cf = getOpt(o, 'caseFirst', ['upper', 'lower', 'false'], 'false'); this._ip = !!o.ignorePunctuation;
    }
    function cls(ch) { if (/\s/.test(ch)) return 0; if (/[_\-,;:!?.'"()\[\]{}@*\/\\&#%`^+<=>|~$]/.test(ch)) return 1; if (/[0-9]/.test(ch)) return 2; return 3; }
    function primaryCmp(a, b, num) {
      var i = 0, j = 0;
      while (i < a.length && j < b.length) {
        if (num && /\d/.test(a[i]) && /\d/.test(b[j])) {
          var ma = /^\d+/.exec(a.slice(i))[0], mb = /^\d+/.exec(b.slice(j))[0];
          var na = ma.replace(/^0+(?=\d)/, ''), nb = mb.replace(/^0+(?=\d)/, '');
          if (na.length !== nb.length) return na.length < nb.length ? -1 : 1;
          if (na !== nb) return na < nb ? -1 : 1;
          i += ma.length; j += mb.length; continue;
        }
        var ca = a[i], cb = b[j];
        if (ca !== cb) { var ka = cls(ca), kb = cls(cb); if (ka !== kb) return ka < kb ? -1 : 1; return ca < cb ? -1 : 1; }
        i++; j++;
      }
      return a.length - i === b.length - j ? 0 : (a.length - i < b.length - j ? -1 : 1);
    }
    Collator.prototype.compare = undefined;
    Object.defineProperty(Collator.prototype, 'compare', { get: function () {
      var self = this; if (this._bound) return this._bound;
      var f = function (x, y) {
        x = String(x); y = String(y);
        var a = x, b = y;
        if (self._ip) { a = a.replace(/[\s\p{P}]/gu, ''); b = b.replace(/[\s\p{P}]/gu, ''); }
        var pa = stripAccents(a).toLowerCase(), pb = stripAccents(b).toLowerCase();
        var c = primaryCmp(pa, pb, self._num); if (c) return c;
        if (self._s === 'base') return 0;
        if (self._s !== 'case') { var aa = a.normalize('NFD').toLowerCase(), ab = b.normalize('NFD').toLowerCase(); if (aa !== ab) { c = primaryCmp(aa, ab, false); if (c) return c; } }
        if (self._s === 'accent') return 0;
        if (self._s === 'case' || self._s === 'variant') { var ba = stripAccents(a), bb = stripAccents(b); if (ba !== bb) { for (var k = 0; k < ba.length; k++) { if (ba[k] !== bb[k]) { var lowerA = ba[k] === ba[k].toLowerCase(); var r = lowerA ? -1 : 1; return self._cf === 'upper' ? -r : r; } } } }
        return 0;
      };
      Object.defineProperty(this, '_bound', { value: f }); return f;
    }, configurable: true });
    Collator.prototype.resolvedOptions = function () { return { locale: this._l, usage: this._u, sensitivity: this._s, ignorePunctuation: this._ip, collation: 'default', numeric: this._num, caseFirst: this._cf }; };
    mk('Collator', Collator);

    // ---------- ListFormat ----------
    function ListFormat(locales, options) {
      if (!(this instanceof ListFormat)) throw new TypeError("Constructor Intl.ListFormat requires 'new'");
      var o = toObj(options); this._l = pickLocale(locales); this._t = getOpt(o, 'type', ['conjunction', 'disjunction', 'unit'], 'conjunction'); this._s = getOpt(o, 'style', ['long', 'short', 'narrow'], 'long');
    }
    ListFormat.prototype.formatToParts = function (list) {
      var a = Array.prototype.map.call(list, function (x) { if (typeof x !== 'string') throw new TypeError('Iterable yielded ' + x + ' which is not a string'); return x; }), t = this._t, s = this._s, out = [];
      var word = t === 'disjunction' ? 'or' : (t === 'unit' ? '' : (s === 'long' ? 'and' : s === 'short' ? '&' : ''));
      if (lang(this._l) === 'es') word = t === 'disjunction' ? 'o' : 'y'; if (lang(this._l) === 'de') word = t === 'disjunction' ? 'oder' : 'und'; if (lang(this._l) === 'fr') word = t === 'disjunction' ? 'ou' : 'et';
      var en = lang(this._l) === 'en', oxford = en && this._l !== 'en-GB' && this._l !== 'en-AU';
      for (var i = 0; i < a.length; i++) {
        out.push({ type: 'element', value: a[i] });
        if (i === a.length - 1) break;
        var last = i === a.length - 2, sep;
        if (!last) sep = t === 'unit' && s === 'narrow' ? ' ' : ', ';
        else if (t === 'unit') sep = s === 'narrow' ? ' ' : ', ';
        else if (!word) sep = ', ';
        else if (a.length === 2) sep = ' ' + word + ' ';
        else sep = (oxford || !en ? (en ? ', ' : ' ') : ' ') + word + ' ';
        if (!en && last && a.length > 2) sep = ' ' + word + ' ';
        out.push({ type: 'literal', value: sep });
      }
      return out;
    };
    ListFormat.prototype.format = function (list) { return this.formatToParts(list).map(function (p) { return p.value; }).join(''); };
    ListFormat.prototype.resolvedOptions = function () { return { locale: this._l, type: this._t, style: this._s }; };
    mk('ListFormat', ListFormat);

    // ---------- Segmenter ----------
    function Segmenter(locales, options) {
      if (!(this instanceof Segmenter)) throw new TypeError("Constructor Intl.Segmenter requires 'new'");
      var o = toObj(options); this._l = pickLocale(locales); this._g = getOpt(o, 'granularity', ['grapheme', 'word', 'sentence'], 'grapheme');
    }
    function segs(str, g) {
      var out = [], re, m;
      if (g === 'grapheme') re = /[\u{1f1e6}-\u{1f1ff}][\u{1f1e6}-\u{1f1ff}]|(?:\P{M}|^)\p{M}*(?:‍\p{Extended_Pictographic}\p{M}*[️\u{1f3fb}-\u{1f3ff}]?)*[️\u{1f3fb}-\u{1f3ff}]?|\r\n|[\s\S]/gu;
      else if (g === 'word') re = /[\p{L}\p{N}_]+(?:['’.][\p{L}\p{N}_]+)*|\s+|[\s\S]/gu;
      else re = /[^.!?\n]*(?:[.!?]+["')\]]*\s*|\n+|$)/gu;
      re.lastIndex = 0;
      while ((m = re.exec(str)) !== null) {
        if (m[0] === '') { if (re.lastIndex >= str.length) break; re.lastIndex++; continue; }
        out.push({ segment: m[0], index: m.index, input: str }); if (g === 'word') out[out.length - 1].isWordLike = /[\p{L}\p{N}]/u.test(m[0]);
      }
      return out;
    }
    Segmenter.prototype.segment = function (str) {
      str = String(str); var g = this._g, cache = null;
      var all = function () { return cache || (cache = segs(str, g)); };
      return { containing: function (i) { i = i === undefined ? 0 : Math.floor(Number(i)); var a = all(); for (var k = 0; k < a.length; k++) if (i >= a[k].index && i < a[k].index + a[k].segment.length) return a[k]; return undefined; }, [Symbol.iterator]: function () { return all()[Symbol.iterator](); } };
    };
    Segmenter.prototype.resolvedOptions = function () { return { locale: this._l, granularity: this._g }; };
    mk('Segmenter', Segmenter);

    // ---------- DisplayNames, Locale ----------
    var REGIONS = { US: 'United States', GB: 'United Kingdom', DE: 'Germany', FR: 'France', ES: 'Spain', IT: 'Italy', JP: 'Japan', CN: 'China', IN: 'India', BR: 'Brazil', CA: 'Canada', AU: 'Australia', MX: 'Mexico', RU: 'Russia', KR: 'South Korea', NL: 'Netherlands', SE: 'Sweden', CH: 'Switzerland', PL: 'Poland', TR: 'Türkiye' };
    var LANGS = { en: 'English', de: 'German', fr: 'French', es: 'Spanish', it: 'Italian', ja: 'Japanese', zh: 'Chinese', pt: 'Portuguese', ru: 'Russian', ko: 'Korean', nl: 'Dutch', sv: 'Swedish', pl: 'Polish', tr: 'Turkish', ar: 'Arabic', hi: 'Hindi', 'en-US': 'American English', 'en-GB': 'British English' };
    function DisplayNames(locales, options) {
      if (!(this instanceof DisplayNames)) throw new TypeError("Constructor Intl.DisplayNames requires 'new'");
      var o = toObj(options); if (o.type === undefined) throw new TypeError('Required option "type" is missing'); this._l = pickLocale(locales); this._t = getOpt(o, 'type', ['language', 'region', 'script', 'currency', 'calendar', 'dateTimeField'], 'language'); this._f = getOpt(o, 'fallback', ['code', 'none'], 'code');
    }
    DisplayNames.prototype.of = function (c) { c = String(c); var t = this._t, v; if (t === 'region') v = REGIONS[c.toUpperCase()]; else if (t === 'language') v = LANGS[c] || LANGS[c.split('-')[0]]; else if (t === 'currency') v = CUR_NAME[c.toUpperCase()] && CUR_NAME[c.toUpperCase()][0].replace(/^./, function (x) { return x.toUpperCase(); }); return v || (this._f === 'code' ? c : undefined); };
    DisplayNames.prototype.resolvedOptions = function () { return { locale: this._l, style: 'long', type: this._t, fallback: this._f }; };
    mk('DisplayNames', DisplayNames);
    function Locale(tag, o) {
      if (!(this instanceof Locale)) throw new TypeError("Constructor Intl.Locale requires 'new'");
      var t = canon(String(tag)), p = t.split('-'); o = o || {};
      this.language = o.language || p[0]; this.script = o.script || p.filter(function (x, i) { return i && x.length === 4; })[0]; this.region = o.region || p.filter(function (x, i) { return i && (x.length === 2 || /^\d{3}$/.test(x)); })[0];
      this.baseName = [this.language, this.script, this.region].filter(Boolean).join('-');
    }
    Locale.prototype.toString = function () { return this.baseName; };
    Locale.prototype.maximize = function () { return this; }; Locale.prototype.minimize = function () { return this; };
    mk('Locale', Locale);

    Object.defineProperty(Intl, 'getCanonicalLocales', { value: function getCanonicalLocales(l) { return locList(l); }, writable: true, configurable: true });
    Object.defineProperty(Intl, 'supportedValuesOf', { value: function supportedValuesOf(k) { var m = { currency: Object.keys(SYM).concat(['CHF', 'RUB', 'SEK', 'NOK', 'DKK', 'PLN', 'TRY', 'ZAR']).sort(), unit: Object.keys(UNITS).sort(), calendar: ['gregory'], collation: ['default'], numberingSystem: ['latn'], timeZone: ['UTC'] }; if (!m[k]) throw new RangeError('Invalid key : ' + k); return m[k]; }, writable: true, configurable: true });
    Object.defineProperty(Intl, Symbol.toStringTag, { value: 'Intl', configurable: true });
    Object.defineProperty(window, 'Intl', { value: Intl, writable: true, configurable: true });

  })();
  // ---- crypto.subtle: digest (SHA-1/256/384/512) and HMAC. Nothing else: no keys that leave the page. -----------
  (function () {
    if (!window.crypto || window.crypto.subtle) return;
    var K256 = new Uint32Array([0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
    function rotr(x, n) { return (x >>> n) | (x << (32 - n)); }
    function pad(msg, block, lenBytes) {
      var n = msg.length, total = Math.ceil((n + 1 + lenBytes) / block) * block, out = new Uint8Array(total);
      out.set(msg); out[n] = 0x80;
      var bits = n * 8, dv = new DataView(out.buffer);
      dv.setUint32(total - 4, bits >>> 0); dv.setUint32(total - 8, Math.floor(bits / 4294967296));
      return out;
    }
    function sha256(msg) {
      var h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]), p = pad(msg, 64, 8), dv = new DataView(p.buffer), w = new Uint32Array(64);
      for (var o = 0; o < p.length; o += 64) {
        for (var i = 0; i < 16; i++) w[i] = dv.getUint32(o + i * 4);
        for (i = 16; i < 64; i++) { var s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10); w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0; }
        var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (i = 0; i < 64; i++) {
          var S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = (hh + S1 + ch + K256[i] + w[i]) | 0, S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = (S0 + mj) | 0;
          hh = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
      }
      var out = new Uint8Array(32), odv = new DataView(out.buffer); for (var j = 0; j < 8; j++) odv.setUint32(j * 4, h[j]); return out;
    }
    function sha1(msg) {
      var h = new Uint32Array([0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0]), p = pad(msg, 64, 8), dv = new DataView(p.buffer), w = new Uint32Array(80);
      function rotl(x, n) { return (x << n) | (x >>> (32 - n)); }
      for (var o = 0; o < p.length; o += 64) {
        for (var i = 0; i < 16; i++) w[i] = dv.getUint32(o + i * 4);
        for (i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (i = 0; i < 80; i++) { var f, k; if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; } else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; } else { f = b ^ c ^ d; k = 0xca62c1d6; } var t = (rotl(a, 5) + f + e + k + w[i]) | 0; e = d; d = c; c = rotl(b, 30); b = a; a = t; }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
      }
      var out = new Uint8Array(20), odv = new DataView(out.buffer); for (var j = 0; j < 5; j++) odv.setUint32(j * 4, h[j]); return out;
    }
    var K512 = null;
    function sha512(msg, is384) {
      var M = (1n << 64n) - 1n;
      if (!K512) { K512 = []; var primes = [], n = 2; while (primes.length < 80) { var isp = true; for (var q = 0; q < primes.length && primes[q] * primes[q] <= n; q++) if (n % primes[q] === 0) { isp = false; break; } if (isp) primes.push(n); n++; }
        var icbrt = function (x) { var lo = 0n, hi = 1n << 70n; while (lo < hi) { var mid = (lo + hi + 1n) >> 1n; if (mid * mid * mid <= x) lo = mid; else hi = mid - 1n; } return lo; };
        for (var i = 0; i < 80; i++) K512.push(icbrt(BigInt(primes[i]) << 192n) & M); K512.primes = primes; }
      var isqrt = function (x) { var lo = 0n, hi = 1n << 70n; while (lo < hi) { var mid = (lo + hi + 1n) >> 1n; if (mid * mid <= x) lo = mid; else hi = mid - 1n; } return lo; };
      var pr = K512.primes, H = [];
      if (is384) for (var i = 8; i < 16; i++) H.push(isqrt(BigInt(pr[i]) << 128n) & M); else for (i = 0; i < 8; i++) H.push(isqrt(BigInt(pr[i]) << 128n) & M);
      var p = pad(msg, 128, 16), dv = new DataView(p.buffer), w = new Array(80);
      var rotr64 = function (x, n) { return ((x >> BigInt(n)) | (x << BigInt(64 - n))) & M; };
      for (var o = 0; o < p.length; o += 128) {
        for (i = 0; i < 16; i++) w[i] = (BigInt(dv.getUint32(o + i * 8)) << 32n) | BigInt(dv.getUint32(o + i * 8 + 4));
        for (i = 16; i < 80; i++) { var s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7n), s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6n); w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & M; }
        var a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], hh = H[7];
        for (i = 0; i < 80; i++) {
          var S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41), ch = (e & f) ^ (~e & M & g), t1 = (hh + S1 + ch + K512[i] + w[i]) & M, S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39), mj = (a & b) ^ (a & c) ^ (b & c), t2 = (S0 + mj) & M;
          hh = g; g = f; f = e; e = (d + t1) & M; d = c; c = b; b = a; a = (t1 + t2) & M;
        }
        H[0] = (H[0] + a) & M; H[1] = (H[1] + b) & M; H[2] = (H[2] + c) & M; H[3] = (H[3] + d) & M; H[4] = (H[4] + e) & M; H[5] = (H[5] + f) & M; H[6] = (H[6] + g) & M; H[7] = (H[7] + hh) & M;
      }
      var len = is384 ? 6 : 8, out = new Uint8Array(len * 8), odv = new DataView(out.buffer);
      for (i = 0; i < len; i++) { odv.setUint32(i * 8, Number(H[i] >> 32n)); odv.setUint32(i * 8 + 4, Number(H[i] & 0xffffffffn)); }
      return out;
    }
    var ALGS = { 'SHA-1': [sha1, 64], 'SHA-256': [sha256, 64], 'SHA-384': [function (m) { return sha512(m, true); }, 128], 'SHA-512': [function (m) { return sha512(m, false); }, 128] };
    function algName(a) { var n = typeof a === 'string' ? a : a && a.name; if (typeof n !== 'string') throw new TypeError("Failed to execute 'digest' on 'SubtleCrypto': Algorithm: Unrecognized name"); var u = n.toUpperCase(); if (!ALGS[u]) throw new DOMException('Unrecognized name.', 'NotSupportedError'); return u; }
    function bytes(d, fn) {
      if (d instanceof ArrayBuffer) return new Uint8Array(d);
      if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
      throw new TypeError("Failed to execute '" + fn + "' on 'SubtleCrypto': parameter 2 is not of type '(ArrayBuffer or ArrayBufferView)'.");
    }
    function hmac(hashName, key, msg) {
      var alg = ALGS[hashName], blk = alg[1], k = key.length > blk ? alg[0](key) : key, ip = new Uint8Array(blk + msg.length), ot;
      for (var i = 0; i < blk; i++) { var kb = i < k.length ? k[i] : 0; ip[i] = kb ^ 0x36; }
      ip.set(msg, blk); var inner = alg[0](ip); ot = new Uint8Array(blk + inner.length);
      for (i = 0; i < blk; i++) ot[i] = (i < k.length ? k[i] : 0) ^ 0x5c; ot.set(inner, blk); return alg[0](ot);
    }
    function CryptoKey() { throw new TypeError('Illegal constructor'); }
    var subtle = {
      digest: function digest(a, d) { return new Promise(function (res) { var n = algName(a); res(ALGS[n][0](bytes(d, 'digest').slice()).buffer); }); },
      importKey: function importKey(fmt, data, algo, ext, usages) {
        return new Promise(function (res) {
          if (fmt !== 'raw') throw new DOMException('Unsupported import key format', 'NotSupportedError');
          var name = (typeof algo === 'string' ? algo : algo.name).toUpperCase();
          if (name !== 'HMAC') throw new DOMException('Unrecognized algorithm name', 'NotSupportedError');
          var k = Object.create(CryptoKey.prototype); k._raw = bytes(data, 'importKey').slice(); k.type = 'secret'; k.extractable = !!ext; k.algorithm = { name: 'HMAC', hash: { name: algName(algo.hash) }, length: k._raw.length * 8 }; k.usages = usages || [];
          res(k);
        });
      },
      sign: function sign(algo, key, data) { return new Promise(function (res) { if (!key || !key._raw) throw new TypeError('key is not a CryptoKey'); res(hmac(key.algorithm.hash.name, key._raw, bytes(data, 'sign')).buffer.slice(0)); }); },
      verify: function verify(algo, key, sig, data) { return subtle.sign(algo, key, data).then(function (m) { var a = new Uint8Array(m), b = bytes(sig, 'verify'); if (a.length !== b.length) return false; var d = 0; for (var i = 0; i < a.length; i++) d |= a[i] ^ b[i]; return d === 0; }); },
      exportKey: function exportKey(fmt, key) { return new Promise(function (res) { if (!key.extractable) throw new DOMException('key is not extractable', 'InvalidAccessError'); res(key._raw.buffer.slice(0)); }); },
      generateKey: function () { return Promise.reject(new DOMException('The operation is not supported', 'NotSupportedError')); },
      encrypt: function () { return Promise.reject(new DOMException('The operation is not supported', 'NotSupportedError')); },
      decrypt: function () { return Promise.reject(new DOMException('The operation is not supported', 'NotSupportedError')); },
      deriveBits: function () { return Promise.reject(new DOMException('The operation is not supported', 'NotSupportedError')); },
      deriveKey: function () { return Promise.reject(new DOMException('The operation is not supported', 'NotSupportedError')); }
    };
    function SubtleCrypto() { throw new TypeError('Illegal constructor'); }
    SubtleCrypto.prototype = Object.getPrototypeOf(subtle) === Object.prototype ? SubtleCrypto.prototype : SubtleCrypto.prototype;
    Object.keys(subtle).forEach(function (k) { Object.defineProperty(SubtleCrypto.prototype, k, { value: subtle[k], writable: true, configurable: true, enumerable: true }); });
    var inst = Object.create(SubtleCrypto.prototype);
    Object.defineProperty(SubtleCrypto.prototype, Symbol.toStringTag, { value: 'SubtleCrypto', configurable: true });
    def('SubtleCrypto', SubtleCrypto); def('CryptoKey', CryptoKey);
    try { Object.defineProperty(Object.getPrototypeOf(window.crypto) === Object.prototype ? window.crypto : Object.getPrototypeOf(window.crypto), 'subtle', { get: function () { return inst; }, configurable: true, enumerable: true }); } catch (e) { Object.defineProperty(window.crypto, 'subtle', { value: inst, configurable: true, enumerable: true }); }
  })();
  // ---- navigator extras, storage stubs, iterator helpers, Atomics, FileReader, DOMParser, TreeWalker ----------
  (function () {
    var nav = window.navigator, NP = Object.getPrototypeOf(nav) || nav;
    function npDef(k, v) { if (!(k in nav)) Object.defineProperty(NP === Object.prototype ? nav : NP, k, { get: function () { return v; }, configurable: true, enumerable: true }); }
    function reject(name, msg) { return Promise.reject(new DOMException(msg, name)); }
    npDef('clipboard', Object.assign(new EventTarget(), { writeText: function (t) { return Promise.resolve(); }, readText: function () { return reject('NotAllowedError', 'Read permission denied.'); }, write: function () { return Promise.resolve(); }, read: function () { return reject('NotAllowedError', 'Read permission denied.'); } }));
    npDef('permissions', { query: function (d) { var s = Object.assign(new EventTarget(), { name: d && d.name, state: 'prompt', onchange: null }); return Promise.resolve(s); } });
    npDef('serviceWorker', Object.assign(new EventTarget(), { controller: null, ready: new Promise(function () { }), register: function () { return reject('SecurityError', 'Failed to register a ServiceWorker: service workers are not available in this browser.'); }, getRegistration: function () { return Promise.resolve(undefined); }, getRegistrations: function () { return Promise.resolve([]); }, startMessages: function () { } }));
    npDef('mediaDevices', Object.assign(new EventTarget(), { enumerateDevices: function () { return Promise.resolve([]); }, getUserMedia: function () { return reject('NotAllowedError', 'Permission denied'); }, getDisplayMedia: function () { return reject('NotAllowedError', 'Permission denied'); }, getSupportedConstraints: function () { return {}; } }));
    npDef('connection', Object.assign(new EventTarget(), { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, type: 'wifi', onchange: null }));
    npDef('storage', { estimate: function () { return Promise.resolve({ quota: 1073741824, usage: 0 }); }, persist: function () { return Promise.resolve(false); }, persisted: function () { return Promise.resolve(false); } });
    npDef('userAgentData', { brands: [{ brand: 'Chromium', version: '120' }, { brand: 'Not_A Brand', version: '24' }], mobile: false, platform: 'Linux', getHighEntropyValues: function (h) { return Promise.resolve({ brands: this.brands, mobile: false, platform: 'Linux', architecture: 'x86', bitness: '64', model: '', platformVersion: '', uaFullVersion: '120.0.0.0' }); }, toJSON: function () { return { brands: this.brands, mobile: false, platform: 'Linux' }; } });
    npDef('locks', { request: function (n, o, cb) { cb = typeof o === 'function' ? o : cb; return Promise.resolve().then(function () { return cb({ name: n, mode: 'exclusive' }); }); }, query: function () { return Promise.resolve({ held: [], pending: [] }); } });
    npDef('credentials', { get: function () { return Promise.resolve(null); }, create: function () { return Promise.resolve(null); }, store: function (c) { return Promise.resolve(c); }, preventSilentAccess: function () { return Promise.resolve(); } });
    npDef('webdriver', false); npDef('doNotTrack', null); npDef('pdfViewerEnabled', false); npDef('deviceMemory', 8);
    npDef('getBattery', function () { return Promise.resolve(Object.assign(new EventTarget(), { charging: true, chargingTime: 0, dischargingTime: Infinity, level: 1 })); });

    // IndexedDB: present but empty; opening fails, as it does in a profile where storage is blocked, so libraries use their fallback.
    if (typeof window.indexedDB === 'undefined') {
      function IDBRequest() { }
      IDBRequest.prototype = Object.create(EventTarget.prototype, { constructor: { value: IDBRequest, writable: true, configurable: true } });
      var failReq = function () {
        var r = new IDBRequest(); r.result = undefined; r.error = null; r.readyState = 'pending'; r.source = null; r.transaction = null; r.onsuccess = r.onerror = r.onblocked = r.onupgradeneeded = null;
        setTimeout(function () { r.readyState = 'done'; r.error = new DOMException('A mutation operation was attempted on a database that did not allow mutations.', 'UnknownError'); var ev = new Event('error', { cancelable: true, bubbles: true }); if (typeof r.onerror === 'function') r.onerror(ev); r.dispatchEvent(ev); }, 0);
        return r;
      };
      def('IDBRequest', IDBRequest); def('IDBOpenDBRequest', IDBRequest);
      function IDBKeyRange() { throw new TypeError('Illegal constructor'); }
      IDBKeyRange.only = function (v) { return { lower: v, upper: v, lowerOpen: false, upperOpen: false }; }; IDBKeyRange.lowerBound = function (v, o) { return { lower: v, lowerOpen: !!o, upperOpen: true }; }; IDBKeyRange.upperBound = function (v, o) { return { upper: v, upperOpen: !!o, lowerOpen: true }; }; IDBKeyRange.bound = function (l, u, lo, uo) { return { lower: l, upper: u, lowerOpen: !!lo, upperOpen: !!uo }; };
      def('IDBKeyRange', IDBKeyRange);
      def('IDBFactory', function IDBFactory() { throw new TypeError('Illegal constructor'); });
      def('IDBDatabase', function IDBDatabase() { throw new TypeError('Illegal constructor'); }); def('IDBTransaction', function IDBTransaction() { throw new TypeError('Illegal constructor'); }); def('IDBObjectStore', function IDBObjectStore() { throw new TypeError('Illegal constructor'); });
      def('indexedDB', { open: failReq, deleteDatabase: failReq, databases: function () { return Promise.resolve([]); }, cmp: function (a, b) { return a < b ? -1 : a > b ? 1 : 0; } });
    }
    if (typeof window.caches === 'undefined') {
      var cache = { match: function () { return Promise.resolve(undefined); }, matchAll: function () { return Promise.resolve([]); }, add: function () { return Promise.resolve(); }, addAll: function () { return Promise.resolve(); }, put: function () { return Promise.resolve(); }, delete: function () { return Promise.resolve(false); }, keys: function () { return Promise.resolve([]); } };
      def('caches', { open: function () { return Promise.resolve(cache); }, match: function () { return Promise.resolve(undefined); }, has: function () { return Promise.resolve(false); }, delete: function () { return Promise.resolve(false); }, keys: function () { return Promise.resolve([]); } });
    }

    // Iterator helpers (ES2025) and friends the engine's iterators already satisfy.
    (function () {
      var IterProto = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
      if (typeof window.Iterator === 'function' && typeof IterProto.map === 'function') return;
      function Iterator() { if (new.target === undefined || new.target === Iterator) throw new TypeError('Abstract class Iterator not directly constructable'); }
      Iterator.prototype = IterProto;
      Object.defineProperty(IterProto, 'constructor', { value: Iterator, writable: true, configurable: true });
      function fn(f) { if (typeof f !== 'function') throw new TypeError(String(f) + ' is not a function'); return f; }
      function helper(src, gen) { var it = gen(); var H = Object.create(HelperProto); H._it = it; H._src = src; return H; }
      var HelperProto = Object.create(IterProto, {});
      HelperProto.next = function () { return this._it.next(); };
      HelperProto.return = function () { var r = this._it.return(); if (this._src && typeof this._src.return === 'function') this._src.return(); return r; };
      Object.defineProperty(HelperProto, Symbol.toStringTag, { value: 'Iterator Helper', configurable: true });
      function direct(o) { return { it: o, next: o.next }; }
      var methods = {
        map: function (f) { fn(f); var s = this, i = 0; return helper(s, function* () { for (var r = s.next(); !r.done; r = s.next()) yield f(r.value, i++); }); },
        filter: function (f) { fn(f); var s = this, i = 0; return helper(s, function* () { for (var r = s.next(); !r.done; r = s.next()) if (f(r.value, i++)) yield r.value; }); },
        take: function (n) { n = Number(n); if (n !== n || n < 0) throw new RangeError(String(n) + ' must be positive'); var s = this; return helper(s, function* () { var k = n; while (k-- > 0) { var r = s.next(); if (r.done) return; yield r.value; } if (typeof s.return === 'function') s.return(); }); },
        drop: function (n) { n = Number(n); if (n !== n || n < 0) throw new RangeError(String(n) + ' must be positive'); var s = this; return helper(s, function* () { var k = n; while (k-- > 0) { var r = s.next(); if (r.done) return; } for (var r2 = s.next(); !r2.done; r2 = s.next()) yield r2.value; }); },
        flatMap: function (f) { fn(f); var s = this, i = 0; return helper(s, function* () { for (var r = s.next(); !r.done; r = s.next()) { var inner = f(r.value, i++); for (var x of inner) yield x; } }); },
        reduce: function (f, init) { fn(f); var acc = init, i = 0, r = this.next(); if (arguments.length < 2) { if (r.done) throw new TypeError('Reduce of empty iterator with no initial value'); acc = r.value; r = this.next(); i = 1; } for (; !r.done; r = this.next()) acc = f(acc, r.value, i++); return acc; },
        toArray: function () { var out = []; for (var r = this.next(); !r.done; r = this.next()) out.push(r.value); return out; },
        forEach: function (f) { fn(f); var i = 0; for (var r = this.next(); !r.done; r = this.next()) f(r.value, i++); },
        some: function (f) { fn(f); var i = 0; for (var r = this.next(); !r.done; r = this.next()) if (f(r.value, i++)) { if (this.return) this.return(); return true; } return false; },
        every: function (f) { fn(f); var i = 0; for (var r = this.next(); !r.done; r = this.next()) if (!f(r.value, i++)) { if (this.return) this.return(); return false; } return true; },
        find: function (f) { fn(f); var i = 0; for (var r = this.next(); !r.done; r = this.next()) if (f(r.value, i++)) { if (this.return) this.return(); return r.value; } return undefined; }
      };
      Object.keys(methods).forEach(function (k) { Object.defineProperty(IterProto, k, { value: methods[k], writable: true, configurable: true }); });
      Iterator.from = function (o) { var m = o[Symbol.iterator]; var it = typeof m === 'function' ? m.call(o) : o; if (Object.getPrototypeOf(it) === IterProto || it instanceof Iterator) return it; var W = Object.create(IterProto); W.next = function () { return it.next(); }; if (typeof it.return === 'function') W.return = function () { return it.return(); }; return W; };
      def('Iterator', Iterator);
    })();
    if (typeof Symbol.dispose === 'undefined') { try { Object.defineProperty(Symbol, 'dispose', { value: Symbol('Symbol.dispose') }); Object.defineProperty(Symbol, 'asyncDispose', { value: Symbol('Symbol.asyncDispose') }); } catch (e) { } }
    if (typeof Array.fromAsync !== 'function') Object.defineProperty(Array, 'fromAsync', { value: async function fromAsync(src, mapFn, thisArg) { var out = [], i = 0; if (src[Symbol.asyncIterator] || src[Symbol.iterator]) { for await (var v of src) out.push(mapFn ? await mapFn.call(thisArg, v, i++) : v); } else { var n = src.length >>> 0; for (; i < n; i++) { var x = await src[i]; out.push(mapFn ? await mapFn.call(thisArg, x, i) : x); } } return out; }, writable: true, configurable: true });
    if (typeof Promise.try !== 'function') Object.defineProperty(Promise, 'try', { value: function (f) { var a = Array.prototype.slice.call(arguments, 1); return new Promise(function (res) { res(f.apply(undefined, a)); }); }, writable: true, configurable: true });
    if (typeof window.Atomics === 'undefined') {
      var At = {};
      function op(n, f) { Object.defineProperty(At, n, { value: function (ta, i, v) { var old = ta[i]; ta[i] = f(old, v); return old; }, writable: true, configurable: true }); }
      op('add', function (a, b) { return a + b; }); op('sub', function (a, b) { return a - b; }); op('and', function (a, b) { return a & b; }); op('or', function (a, b) { return a | b; }); op('xor', function (a, b) { return a ^ b; }); op('exchange', function (a, b) { return b; });
      Object.defineProperty(At, 'load', { value: function (ta, i) { return ta[i]; }, writable: true, configurable: true });
      Object.defineProperty(At, 'store', { value: function (ta, i, v) { ta[i] = v; return v; }, writable: true, configurable: true });
      Object.defineProperty(At, 'compareExchange', { value: function (ta, i, e, r) { var o = ta[i]; if (o === e) ta[i] = r; return o; }, writable: true, configurable: true });
      Object.defineProperty(At, 'isLockFree', { value: function (n) { return n === 1 || n === 2 || n === 4 || n === 8; }, writable: true, configurable: true });
      Object.defineProperty(At, 'notify', { value: function () { return 0; }, writable: true, configurable: true });
      Object.defineProperty(At, 'wait', { value: function () { return 'timed-out'; }, writable: true, configurable: true });
      Object.defineProperty(At, Symbol.toStringTag, { value: 'Atomics', configurable: true });
      def('Atomics', At);
    }

    // FileReader over Blob.text()/arrayBuffer()
    if (typeof window.FileReader !== 'function') {
      var FR = class FileReader extends EventTarget {
        constructor() { super(); this.readyState = 0; this.result = null; this.error = null; this.onload = this.onloadend = this.onerror = this.onloadstart = this.onprogress = this.onabort = null; }
        _run(blob, how) {
          if (this.readyState === 1) throw new DOMException("Failed to execute 'readAs' on 'FileReader': The object is already busy reading Blobs.", 'InvalidStateError');
          var s = this; s.readyState = 1; s.result = null;
          function fire(t) { var ev = new ProgressEvent(t, { lengthComputable: false, loaded: 0, total: 0 }); var h = s['on' + t]; if (typeof h === 'function') h.call(s, ev); s.dispatchEvent(ev); }
          setTimeout(function () { fire('loadstart'); }, 0);
          Promise.resolve(how(blob)).then(function (r) { setTimeout(function () { if (s.readyState !== 1) return; s.readyState = 2; s.result = r; fire('load'); fire('loadend'); }, 0); }, function (e) { setTimeout(function () { s.readyState = 2; s.error = e; fire('error'); fire('loadend'); }, 0); });
        }
        readAsText(b) { this._run(b, function (x) { return x.text(); }); }
        readAsArrayBuffer(b) { this._run(b, function (x) { return x.arrayBuffer(); }); }
        readAsBinaryString(b) { this._run(b, function (x) { return x.arrayBuffer().then(function (ab) { var u = new Uint8Array(ab), s = ''; for (var i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; }); }); }
        readAsDataURL(b) { this._run(b, function (x) { return x.arrayBuffer().then(function (ab) { var u = new Uint8Array(ab), s = ''; for (var i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return 'data:' + (x.type || 'application/octet-stream') + ';base64,' + btoa(s); }); }); }
        abort() { if (this.readyState === 1) { this.readyState = 2; } }
      };
      ['EMPTY', 'LOADING', 'DONE'].forEach(function (k, i) { Object.defineProperty(FR, k, { value: i, enumerable: true }); Object.defineProperty(FR.prototype, k, { value: i, enumerable: true }); });
      def('FileReader', FR);
    }


    // document.implementation: createHTMLDocument builds a detached <html> tree behind a small Document-shaped object,
    // which is what DOMParser, DOMPurify-style sanitizers and template engines read from.
    if (!document.implementation) {
      var detached = function (title) {
        var html = document.createElement('html'), head = document.createElement('head'), body = document.createElement('body');
        html.appendChild(head); html.appendChild(body);
        var t = document.createElement('title'); t.textContent = title === undefined ? '' : String(title); if (title !== undefined) head.appendChild(t);
        var d = { nodeType: 9, nodeName: '#document', documentElement: html, head: head, body: body, contentType: 'text/html', characterSet: 'UTF-8', compatMode: 'CSS1Compat', URL: 'about:blank', documentURI: 'about:blank', defaultView: null, doctype: null, readyState: 'complete', childNodes: [html], firstChild: html, lastChild: html, children: [html], firstElementChild: html };
        Object.defineProperty(d, 'title', { get: function () { var x = html.querySelector('title'); return x ? x.textContent : ''; }, set: function (v) { var x = html.querySelector('title'); if (!x) { x = document.createElement('title'); head.appendChild(x); } x.textContent = v; }, enumerable: true });
        ['querySelector', 'querySelectorAll', 'getElementsByTagName', 'getElementsByClassName'].forEach(function (m) { d[m] = function () { return html[m].apply(html, arguments); }; });
        d.getElementById = function (id) { var all = html.querySelectorAll('[id]'); for (var i = 0; i < all.length; i++) if (all[i].id === String(id)) return all[i]; return null; };
        ['createElement', 'createElementNS', 'createTextNode', 'createComment', 'createDocumentFragment', 'createRange', 'createTreeWalker', 'createNodeIterator', 'createEvent'].forEach(function (m) { if (document[m]) d[m] = function () { return document[m].apply(document, arguments); }; });
        d.importNode = function (n, deep) { return n.cloneNode(!!deep); }; d.adoptNode = function (n) { return n; };
        d.implementation = document.implementation; d.addEventListener = function () { }; d.removeEventListener = function () { }; d.hasFocus = function () { return false; };
        d.contains = function (n) { return html.contains(n); }; d.write = function () { }; d.open = function () { return d; }; d.close = function () { };
        Object.defineProperty(d, 'ownerDocument', { value: null });
        return d;
      };
      var DI = function DOMImplementation() { throw new TypeError('Illegal constructor'); };
      DI.prototype.createHTMLDocument = function createHTMLDocument(title) { return detached(title); };
      DI.prototype.createDocument = function createDocument(ns, qn, dt) { var d = detached(); if (qn) { var root = document.createElement(String(qn)); d.documentElement.replaceWith ? d.documentElement.replaceWith(root) : 0; } return d; };
      DI.prototype.createDocumentType = function (n, p, s) { return { nodeType: 10, name: n, publicId: p || '', systemId: s || '', nodeName: n }; };
      DI.prototype.hasFeature = function () { return true; };
      def('DOMImplementation', DI);
      Object.defineProperty(Object.getPrototypeOf(document), 'implementation', { value: Object.create(DI.prototype), configurable: true, enumerable: true, writable: true });
    }

    // DOMParser / XMLSerializer over innerHTML and outerHTML
    if (typeof window.DOMParser !== 'function') {
      def('DOMParser', class DOMParser { parseFromString(str, type) {
        type = String(type); if (['text/html', 'text/xml', 'application/xml', 'application/xhtml+xml', 'image/svg+xml'].indexOf(type) < 0) throw new TypeError("Failed to execute 'parseFromString' on 'DOMParser': The provided value '" + type + "' is not a valid enum value of type DOMParserSupportedType.");
        var d = document.implementation.createHTMLDocument('');
        if (type === 'text/html') { if (/<html[\s>]/i.test(str)) d.documentElement.innerHTML = String(str).replace(/^[\s\S]*?<html[^>]*>|<\/html>[\s\S]*$/gi, ''); else d.body.innerHTML = String(str); }
        else { d.body.innerHTML = String(str); }
        return d;
      } });
      def('XMLSerializer', class XMLSerializer { serializeToString(n) { if (n && n.documentElement && n.nodeType === 9) return n.documentElement.outerHTML; if (n && n.outerHTML !== undefined) return n.outerHTML; if (n && n.nodeType === 3) return n.data; if (n && n.nodeType === 11) return Array.prototype.map.call(n.childNodes, function (c) { return c.outerHTML !== undefined ? c.outerHTML : c.data; }).join(''); return ''; } });
    }

    // NodeFilter / TreeWalker / NodeIterator, walked over the live DOM
    if (typeof window.NodeFilter !== 'object' && typeof window.NodeFilter !== 'function') {
      var NF = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_PROCESSING_INSTRUCTION: 64, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024 };
      def('NodeFilter', NF);
      var accept = function (w, n) { if (!((w.whatToShow >>> 0) & (1 << (n.nodeType - 1)))) return 3; var f = w.filter; if (!f) return 1; var r = typeof f === 'function' ? f(n) : f.acceptNode(n); return r; };
      var TW = function TreeWalker(root, what, filter) { this.root = root; this.whatToShow = what === undefined ? 0xFFFFFFFF : what >>> 0; this.filter = filter || null; this.currentNode = root; };
      var nextIn = function (w, n, sibling) { return sibling ? n.nextSibling : n.previousSibling; };
      TW.prototype.parentNode = function () { var n = this.currentNode; while (n && n !== this.root) { n = n.parentNode; if (n && accept(this, n) === 1) return this.currentNode = n; } return null; };
      function child(w, first) { var n = first ? w.currentNode.firstChild : w.currentNode.lastChild; while (n) { var r = accept(w, n); if (r === 1) return w.currentNode = n; if (r === 3) { var c = first ? n.firstChild : n.lastChild; if (c) { n = c; continue; } } while (n) { var s = first ? n.nextSibling : n.previousSibling; if (s) { n = s; break; } var p = n.parentNode; if (!p || p === w.root || p === w.currentNode) return null; n = p; } } return null; }
      TW.prototype.firstChild = function () { return child(this, true); }; TW.prototype.lastChild = function () { return child(this, false); };
      function sib(w, next) { var n = w.currentNode; if (n === w.root) return null; for (;;) { var s = next ? n.nextSibling : n.previousSibling; while (s) { var r = accept(w, s); if (r === 1) return w.currentNode = s; s = r === 2 ? (next ? s.nextSibling : s.previousSibling) : ((next ? s.firstChild : s.lastChild) || (next ? s.nextSibling : s.previousSibling)); } n = n.parentNode; if (!n || n === w.root) return null; if (accept(w, n) === 1) return null; } }
      TW.prototype.nextSibling = function () { return sib(this, true); }; TW.prototype.previousSibling = function () { return sib(this, false); };
      TW.prototype.nextNode = function () { var n = this.currentNode, res = 3; for (;;) { while (res !== 2 && n.firstChild) { n = n.firstChild; res = accept(this, n); if (res === 1) return this.currentNode = n; } var t = n; while (t && t !== this.root) { if (t.nextSibling) { n = t.nextSibling; break; } t = t.parentNode; n = null; } if (!t || t === this.root || !n) return null; res = accept(this, n); if (res === 1) return this.currentNode = n; } };
      TW.prototype.previousNode = function () { var n = this.currentNode; while (n !== this.root) { var s = n.previousSibling; while (s) { n = s; var r = accept(this, n); while (r !== 2 && n.lastChild) { n = n.lastChild; r = accept(this, n); } if (r === 1) return this.currentNode = n; s = n.previousSibling; } if (n === this.root || !n.parentNode) return null; n = n.parentNode; if (accept(this, n) === 1) return this.currentNode = n; } return null; };
      def('TreeWalker', TW);
      var NI = function NodeIterator(root, what, filter) { this.root = root; this.whatToShow = what === undefined ? 0xFFFFFFFF : what >>> 0; this.filter = filter || null; this.referenceNode = root; this.pointerBeforeReferenceNode = true; this._w = new TW(root, what, filter); this._started = false; };
      NI.prototype.nextNode = function () { var w = this._w; w.currentNode = this.referenceNode; if (!this._started) { this._started = true; if (accept(w, this.root) === 1) return this.referenceNode = this.root; } var n = w.nextNode(); if (n) this.referenceNode = n; return n; };
      NI.prototype.previousNode = function () { var w = this._w; w.currentNode = this.referenceNode; var n = w.previousNode(); if (n) this.referenceNode = n; return n; };
      NI.prototype.detach = function () { };
      def('NodeIterator', NI);
      if (!document.createTreeWalker) { document.createTreeWalker = function (r, w, f) { return new TW(r, w, f); }; document.createNodeIterator = function (r, w, f) { return new NI(r, w, f); }; }
    }

    if (typeof window.FontFace !== 'function') {
      def('FontFace', class FontFace { constructor(family, source, desc) { this.family = family; this.source = source; Object.assign(this, { style: 'normal', weight: 'normal', stretch: 'normal', unicodeRange: 'U+0-10FFFF', display: 'auto', status: 'unloaded' }, desc); this.loaded = Promise.resolve(this); } load() { this.status = 'loaded'; return this.loaded; } });
    }
  })();
})(globalThis);
)JS";
