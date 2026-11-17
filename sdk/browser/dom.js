/*
 * The DOM for page scripts, built on the browser's native calls (N, see
 * script.c). Evaluated once per page; returns the hooks script.c calls.
 * Nodes are wrapper objects made by script.c with HTMLElement.prototype
 * or Text.prototype; this file never makes them itself.
 */
(function (N) {
'use strict';
const G = globalThis;
const LISTENERS = Symbol('listeners');
const HANDLERS = Symbol('handlers');
const define = (object, name, value) =>
    Object.defineProperty(object, name, {value, writable: true,
                                         configurable: true});
const illegal = () => { throw new TypeError('Illegal constructor'); };

/* ------------------------------------------------------------ console */

function show(value, depth) {
    if (typeof value === 'string') return depth ? JSON.stringify(value) : value;
    if (value instanceof Node) return '<' + value.nodeName.toLowerCase() + '>';
    if (value instanceof Error) return value.name + ': ' + value.message;
    if (typeof value === 'function') return 'function ' + (value.name || '');
    if (value && typeof value === 'object' && depth < 2) {
        try {
            if (Array.isArray(value))
                return '[' + value.slice(0, 20).map(v => show(v, depth + 1))
                    .join(', ') + (value.length > 20 ? ', ...' : '') + ']';
            const keys = Object.keys(value).slice(0, 20);
            return '{' + keys.map(k => k + ': ' + show(value[k], depth + 1))
                .join(', ') + '}';
        } catch (e) { /* fall through */ }
    }
    return String(value);
}
function format(args) {
    let parts = Array.from(args);
    if (typeof parts[0] === 'string' && /%[sdifoOc]/.test(parts[0])) {
        let i = 1;
        const first = parts[0].replace(/%([sdifoOc%])/g, (m, f) => {
            if (f === '%') return '%';
            if (i >= parts.length) return m;
            const v = parts[i++];
            if (f === 'c') return '';
            if (f === 'd' || f === 'i') return String(parseInt(v));
            if (f === 'f') return String(Number(v));
            return show(v, f === 's' ? 0 : 1);
        });
        parts = [first].concat(parts.slice(i));
    }
    return parts.map(v => show(v, 0)).join(' ');
}
const counts = new Map(), timers = new Map();
const console = {};
for (const level of ['log', 'info', 'warn', 'error', 'debug', 'trace'])
    console[level] = function () { N.log(level, format(arguments)); };
console.dir = console.dirxml = console.table = console.log;
console.group = console.groupCollapsed = console.log;
console.groupEnd = function () {};
console.assert = function (ok, ...rest) {
    if (!ok) N.log('error', 'Assertion failed: ' + format(rest));
};
console.count = function (label = 'default') {
    counts.set(label, (counts.get(label) || 0) + 1);
    N.log('log', label + ': ' + counts.get(label));
};
console.countReset = function (label = 'default') { counts.delete(label); };
console.time = function (label = 'default') { timers.set(label, N.now()); };
console.timeLog = function (label = 'default') {
    if (timers.has(label))
        N.log('log', label + ': ' + (N.now() - timers.get(label)) + ' ms');
};
console.timeEnd = function (label = 'default') {
    console.timeLog(label);
    timers.delete(label);
};
console.clear = function () {};

/* ------------------------------------------------------------- events */

class Event {
    constructor(type, init) {
        init = init || {};
        this.type = String(type);
        this.bubbles = !!init.bubbles;
        this.cancelable = !!init.cancelable;
        this.composed = !!init.composed;
        this.defaultPrevented = false;
        this.target = null;
        this.currentTarget = null;
        this.eventPhase = 0;
        this.isTrusted = false;
        this.timeStamp = N.now();
        this._stop = false;
        this._stopNow = false;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = this._stopNow = true; }
    composedPath() { return this._path ? this._path.slice() : []; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get srcElement() { return this.target; }
    get cancelBubble() { return this._stop; }
    set cancelBubble(v) { if (v) this._stop = true; }
    initEvent(type, bubbles, cancelable) {
        this.type = String(type);
        this.bubbles = !!bubbles;
        this.cancelable = !!cancelable;
    }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2;
Event.BUBBLING_PHASE = 3;
class CustomEvent extends Event {
    constructor(type, init) {
        super(type, init);
        this.detail = init && 'detail' in init ? init.detail : null;
    }
    initCustomEvent(type, bubbles, cancelable, detail) {
        this.initEvent(type, bubbles, cancelable);
        this.detail = detail;
    }
}
class UIEvent extends Event {
    constructor(type, init) {
        super(type, init);
        this.view = G;
        this.detail = (init && init.detail) || 0;
    }
}
class MouseEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        for (const k of ['clientX', 'clientY', 'screenX', 'screenY',
                         'button', 'buttons'])
            this[k] = init[k] || 0;
        for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey'])
            this[k] = !!init[k];
        this.relatedTarget = init.relatedTarget || null;
    }
    get pageX() { return this.clientX + G.scrollX; }
    get pageY() { return this.clientY + G.scrollY; }
    get x() { return this.clientX; }
    get y() { return this.clientY; }
    get offsetX() { return this.clientX; }
    get offsetY() { return this.clientY; }
    get which() { return this.button + 1; }
}
class PointerEvent extends MouseEvent {}
class WheelEvent extends MouseEvent {}
class FocusEvent extends UIEvent {}
class InputEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        this.data = (init && init.data) || null;
        this.inputType = (init && init.inputType) || '';
    }
}
class KeyboardEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        this.key = init.key || '';
        this.code = init.code || '';
        this.keyCode = init.keyCode || 0;
        this.charCode = init.charCode || 0;
        this.location = 0;
        this.repeat = !!init.repeat;
        for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey'])
            this[k] = !!init[k];
    }
    get which() { return this.keyCode; }
    getModifierState(k) {
        return k === 'Control' ? this.ctrlKey : k === 'Shift' ? this.shiftKey
            : k === 'Alt' ? this.altKey : k === 'Meta' ? this.metaKey : false;
    }
}
class SubmitEvent extends Event {}
class ErrorEvent extends Event {}
class ProgressEvent extends Event {}
class PopStateEvent extends Event {
    constructor(type, init) { super(type, init); this.state = init && init.state; }
}
class HashChangeEvent extends Event {}

class EventTarget {
    addEventListener(type, listener, options) {
        if (!listener) return;
        const capture = typeof options === 'boolean' ? options
            : !!(options && options.capture);
        const once = !!(options && typeof options === 'object' && options.once);
        let map = this[LISTENERS];
        if (!map) define(this, LISTENERS, map = new Map());
        let list = map.get(type);
        if (!list) map.set(type, list = []);
        if (list.some(l => l.listener === listener && l.capture === capture))
            return;
        const signal = options && typeof options === 'object' && options.signal;
        const entry = {listener, capture, once};
        list.push(entry);
        if (signal) signal.addEventListener('abort',
            () => this.removeEventListener(type, listener, options));
    }
    removeEventListener(type, listener, options) {
        const capture = typeof options === 'boolean' ? options
            : !!(options && options.capture);
        const list = this[LISTENERS] && this[LISTENERS].get(type);
        if (!list) return;
        const i = list.findIndex(l => l.listener === listener
                                      && l.capture === capture);
        if (i >= 0) list[i].removed = true, list.splice(i, 1);
    }
    dispatchEvent(event) {
        return dispatch(this, event);
    }
}

function parentTarget(target) {
    if (target === G) return null;
    if (target === document) return G;
    if (target instanceof Node) {
        const p = N.parent(target);
        if (p) return p;
        if (target === document.documentElement) return document;
    }
    return null;
}

/* Inline handlers: onclick="..." attributes and el.onclick = f. */
function handlerOf(target, type) {
    const own = target[HANDLERS] && target[HANDLERS][type];
    if (own !== undefined) return own;
    let node = target === G || target === document ? null : target;
    if (target === G && (type === 'load' || type === 'unload'
                         || type === 'resize' || type === 'scroll'))
        node = N.body();
    if (!(node instanceof Element)) return null;
    const code = N.attr(node, 'on' + type);
    if (code == null) return null;
    let fn = null;
    try {
        fn = new Function('event', code);
    } catch (e) {
        N.log('error', 'Uncaught ' + e);
    }
    if (!target[HANDLERS]) define(target, HANDLERS, {});
    target[HANDLERS][type] = fn;
    return fn;
}

/* which: 1 capturing listeners, 2 all (at the target), 3 bubbling ones;
   the on... handler runs with 2 and 3. */
function invoke(target, event, which) {
    const map = target[LISTENERS];
    const list = map && map.get(event.type);
    event.currentTarget = target;
    if (list) {
        for (const entry of list.slice()) {
            if (entry.removed || (which === 1 && !entry.capture)
                || (which === 3 && entry.capture)) continue;
            if (entry.once) target.removeEventListener(event.type,
                entry.listener, entry.capture);
            try {
                const l = entry.listener;
                if (typeof l === 'function') l.call(target, event);
                else if (l && typeof l.handleEvent === 'function')
                    l.handleEvent(event);
            } catch (e) {
                reportError(e);
            }
            if (event._stopNow) break;
        }
    }
    if (which !== 1 && !event._stopNow) {
        const handler = handlerOf(target, event.type);
        if (typeof handler === 'function') {
            try {
                const result = handler.call(target, event);
                if (result === false) event.preventDefault();
            } catch (e) {
                reportError(e);
            }
        }
    }
}

function dispatch(target, event) {
    if (!(event instanceof Event)) throw new TypeError('not an Event');
    event.target = target;
    const path = [];
    for (let t = target; t; t = parentTarget(t)) path.push(t);
    event._path = path;
    event._stop = event._stopNow = false;
    for (let i = path.length - 1; i > 0 && !event._stop; i--) {
        event.eventPhase = 1;
        invoke(path[i], event, 1);
    }
    if (!event._stop) {
        event.eventPhase = 2;
        invoke(target, event, 2);
    }
    if (event.bubbles)
        for (let i = 1; i < path.length && !event._stop; i++) {
            event.eventPhase = 3;
            invoke(path[i], event, 3);
        }
    event.eventPhase = 0;
    event.currentTarget = null;
    return !event.defaultPrevented;
}

function reportError(e) {
    const text = e && e.stack ? e + '\n' + e.stack : String(e);
    N.log('error', 'Uncaught ' + text);
    if (G !== undefined) {
        const event = new ErrorEvent('error');
        event.message = String(e && e.message || e);
        event.error = e;
        try { invoke(G, event, 2); } catch (x) { /* ignore */ }
    }
}

const EVENT_NAMES = ['abort', 'blur', 'change', 'click', 'contextmenu',
    'dblclick', 'error', 'focus', 'input', 'keydown', 'keypress', 'keyup',
    'load', 'mousedown', 'mouseenter', 'mouseleave', 'mousemove',
    'mouseout', 'mouseover', 'mouseup', 'reset', 'resize', 'scroll',
    'select', 'submit', 'unload', 'wheel', 'pointerdown', 'pointerup',
    'touchstart', 'touchend', 'beforeunload', 'hashchange', 'popstate',
    'message', 'readystatechange', 'transitionend', 'animationend',
    'DOMContentLoaded'];
function addHandlerProperties(proto) {
    for (const type of EVENT_NAMES) {
        Object.defineProperty(proto, 'on' + type.toLowerCase(), {
            get() { return handlerOf(this, type); },
            set(fn) {
                if (!this[HANDLERS]) define(this, HANDLERS, {});
                this[HANDLERS][type] = typeof fn === 'function' ? fn : null;
            },
            configurable: true
        });
    }
}

/* -------------------------------------------------------------- nodes */

function toArray(list) {
    define(list, 'item', function (i) { return this[i] === undefined ? null
                                                                    : this[i]; });
    return list;
}
function children(node) {
    const out = [];
    for (let c = N.first(node); c; c = N.next(c)) out.push(c);
    return out;
}
function elementChildren(node) {
    const out = [];
    for (let c = N.first(node); c; c = N.next(c))
        if (N.kind(c) === 1) out.push(c);
    return out;
}
function isFragment(node) {
    return node instanceof Node && N.kind(node) === 1
        && N.name(node) === '#fragment';
}
function toNode(value) {
    if (value instanceof Node) return value;
    return N.createText(String(value));
}
function insert(parent, node, before) {
    if (parent === document) parent = document.documentElement;
    if (!(node instanceof Node))
        throw new TypeError('Argument is not a Node');
    if (before !== null && before !== undefined
        && !(before instanceof Node))
        throw new TypeError('Argument is not a Node');
    if (isFragment(node)) {
        for (const c of children(node)) N.insert(parent, c, before || null);
        return node;
    }
    N.insert(parent, node, before || null);
    return node;
}
const CAMEL = s => s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
const KEBAB = s => s.startsWith('--') ? s
    : s.replace(/[A-Z]/g, c => '-' + c.toLowerCase())
       .replace(/^(webkit|moz|ms)-/, '-$1-');

class Node extends EventTarget {
    constructor() { super(); illegal(); }
    get nodeType() { return N.kind(this); }
    get nodeName() {
        return N.kind(this) === 3 ? '#text' : N.name(this).toUpperCase();
    }
    get parentNode() {
        const p = N.parent(this);
        if (p) return p;
        return this === document.documentElement ? document : null;
    }
    get parentElement() { return N.parent(this); }
    get firstChild() { return N.first(this); }
    get lastChild() { return N.last(this); }
    get nextSibling() { return N.next(this); }
    get previousSibling() { return N.prev(this); }
    get childNodes() { return toArray(children(this)); }
    get ownerDocument() { return document; }
    get isConnected() { return N.connected(this); }
    get textContent() {
        return N.kind(this) === 3 ? N.data(this) : N.text(this);
    }
    set textContent(value) {
        if (N.kind(this) === 3) return N.setData(this, String(value ?? ''));
        for (const c of children(this)) N.remove(c);
        const text = String(value ?? '');
        if (text) N.insert(this, N.createText(text), null);
    }
    get nodeValue() { return N.kind(this) === 3 ? N.data(this) : null; }
    set nodeValue(v) { if (N.kind(this) === 3) N.setData(this, String(v)); }
    hasChildNodes() { return N.first(this) !== null; }
    appendChild(node) { return insert(this, node, null); }
    insertBefore(node, before) { return insert(this, node, before); }
    removeChild(node) {
        if (!(node instanceof Node) || N.parent(node) !== this)
            throw new Error('NotFoundError: not a child of this node');
        N.remove(node);
        return node;
    }
    replaceChild(node, old) {
        if (N.parent(old) !== this)
            throw new Error('NotFoundError: not a child of this node');
        const next = N.next(old);
        N.remove(old);
        insert(this, node, next);
        return old;
    }
    cloneNode(deep) { return N.clone(this, !!deep); }
    contains(other) {
        for (let n = other; n; n = N.parent(n)) if (n === this) return true;
        return false;
    }
    isSameNode(other) { return this === other; }
    isEqualNode(other) {
        return other instanceof Node && N.html(this, true) === N.html(other, true);
    }
    getRootNode() {
        let n = this;
        while (N.parent(n)) n = N.parent(n);
        return n === document.documentElement ? document : n;
    }
    compareDocumentPosition(other) {
        if (other === this) return 0;
        if (this.contains(other)) return 20;
        if (other.contains(this)) return 10;
        const all = N.query(N.root(), '*', true);
        return all.indexOf(other) > all.indexOf(this) ? 4 : 2;
    }
    normalize() {}
}
Node.ELEMENT_NODE = 1; Node.ATTRIBUTE_NODE = 2; Node.TEXT_NODE = 3;
Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9;
Node.DOCUMENT_FRAGMENT_NODE = 11;
Node.DOCUMENT_POSITION_DISCONNECTED = 1;
Node.DOCUMENT_POSITION_PRECEDING = 2;
Node.DOCUMENT_POSITION_FOLLOWING = 4;
Node.DOCUMENT_POSITION_CONTAINS = 8;
Node.DOCUMENT_POSITION_CONTAINED_BY = 16;
for (const k of Object.keys(Node)) define(Node.prototype, k, Node[k]);

const childMixin = {
    remove() { N.remove(this); },
    before(...nodes) {
        const p = N.parent(this);
        if (p) for (const n of nodes) insert(p, toNode(n), this);
    },
    after(...nodes) {
        const p = N.parent(this);
        let next = N.next(this);
        if (p) for (const n of nodes) insert(p, toNode(n), next);
    },
    replaceWith(...nodes) {
        const p = N.parent(this);
        if (!p) return;
        const next = N.next(this);
        N.remove(this);
        for (const n of nodes) insert(p, toNode(n), next);
    },
};

class CharacterData extends Node {
    get data() { return N.data(this); }
    set data(v) { N.setData(this, String(v)); }
    get length() { return N.data(this).length; }
    get nextElementSibling() { return nextElement(this, N.next); }
    get previousElementSibling() { return nextElement(this, N.prev); }
    appendData(s) { N.setData(this, N.data(this) + s); }
    substringData(o, c) { return N.data(this).substr(o, c); }
}
Object.assign(CharacterData.prototype, childMixin);
class Text extends CharacterData {
    get wholeText() { return N.data(this); }
    splitText(offset) {
        const d = N.data(this);
        N.setData(this, d.slice(0, offset));
        const rest = N.createText(d.slice(offset));
        if (N.parent(this)) N.insert(N.parent(this), rest, N.next(this));
        return rest;
    }
}
class Comment extends CharacterData {}

function nextElement(node, step) {
    for (let n = step(node); n; n = step(n)) if (N.kind(n) === 1) return n;
    return null;
}

/* class="..." as a token list. */
class DOMTokenList {
    constructor(element, attribute) {
        define(this, '_e', element);
        define(this, '_a', attribute);
    }
    _get() {
        return (N.attr(this._e, this._a) || '').split(/\s+/).filter(Boolean);
    }
    _set(list) { N.setAttr(this._e, this._a, list.join(' ')); }
    get length() { return this._get().length; }
    get value() { return N.attr(this._e, this._a) || ''; }
    set value(v) { N.setAttr(this._e, this._a, String(v)); }
    item(i) { const l = this._get(); return i < l.length ? l[i] : null; }
    contains(t) { return this._get().includes(String(t)); }
    add(...tokens) {
        const l = this._get();
        let changed = false;
        for (const t of tokens) if (!l.includes(t)) l.push(t), changed = true;
        if (changed || N.attr(this._e, this._a) === null) this._set(l);
    }
    remove(...tokens) {
        const l = this._get();
        const kept = l.filter(t => !tokens.includes(t));
        if (kept.length !== l.length) this._set(kept);
    }
    toggle(t, force) {
        const has = this.contains(t);
        const want = force === undefined ? !has : !!force;
        if (want && !has) this.add(t);
        if (!want && has) this.remove(t);
        return want;
    }
    replace(a, b) {
        const l = this._get();
        const i = l.indexOf(a);
        if (i < 0) return false;
        l[i] = b;
        this._set(l.filter((t, k) => l.indexOf(t) === k));
        return true;
    }
    supports() { return true; }
    forEach(fn, self) { this._get().forEach((t, i) => fn.call(self, t, i, this)); }
    entries() { return this._get().entries(); }
    keys() { return this._get().keys(); }
    values() { return this._get().values(); }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
    toString() { return this.value; }
}

/* style="..." as a declaration object: element.style.color = 'red'. */
function parseStyle(text) {
    const map = new Map();
    for (const part of (text || '').split(';')) {
        const colon = part.indexOf(':');
        if (colon < 0) continue;
        const name = part.slice(0, colon).trim().toLowerCase();
        const value = part.slice(colon + 1).trim();
        if (name) map.set(name, value);
    }
    return map;
}
function styleFor(element) {
    const read = () => parseStyle(N.attr(element, 'style'));
    const write = map => {
        const text = Array.from(map, ([k, v]) => k + ': ' + v).join('; ');
        if (text) N.setAttr(element, 'style', text);
        else N.removeAttr(element, 'style');
    };
    const api = {
        getPropertyValue(name) { return read().get(KEBAB(name)) || ''; },
        setProperty(name, value, priority) {
            const map = read();
            const key = KEBAB(String(name));
            if (value === null || value === undefined || value === '')
                map.delete(key);
            else map.set(key, String(value)
                + (priority === 'important' ? ' !important' : ''));
            write(map);
        },
        removeProperty(name) {
            const map = read();
            const key = KEBAB(String(name));
            const old = map.get(key) || '';
            map.delete(key);
            write(map);
            return old;
        },
        getPropertyPriority(name) {
            return /!important$/.test(read().get(KEBAB(name)) || '')
                ? 'important' : '';
        },
        item(i) { return Array.from(read().keys())[i] || ''; },
    };
    return new Proxy(api, {
        get(target, key) {
            if (typeof key !== 'string') return undefined;
            if (key in target) return target[key];
            if (key === 'cssText') return N.attr(element, 'style') || '';
            if (key === 'length') return read().size;
            if (/^\d+$/.test(key)) return target.item(+key);
            if (key === 'cssFloat') key = 'float';
            return read().get(KEBAB(key)) || '';
        },
        set(target, key, value) {
            if (typeof key !== 'string') return true;
            if (key === 'cssText') {
                N.setAttr(element, 'style', String(value));
                return true;
            }
            if (key === 'cssFloat') key = 'float';
            target.setProperty(key, value);
            return true;
        },
        has(target, key) { return typeof key === 'string'; },
    });
}

function datasetFor(element) {
    return new Proxy({}, {
        get(t, key) {
            if (typeof key !== 'string') return undefined;
            const v = N.attr(element, 'data-' + KEBAB(key));
            return v === null ? undefined : v;
        },
        set(t, key, value) {
            N.setAttr(element, 'data-' + KEBAB(String(key)), String(value));
            return true;
        },
        deleteProperty(t, key) {
            N.removeAttr(element, 'data-' + KEBAB(String(key)));
            return true;
        },
        has(t, key) {
            return N.attr(element, 'data-' + KEBAB(String(key))) !== null;
        },
        ownKeys() {
            return N.attrNames(element).filter(n => n.startsWith('data-'))
                .map(n => CAMEL(n.slice(5)));
        },
        getOwnPropertyDescriptor(t, key) {
            const v = N.attr(element, 'data-' + KEBAB(String(key)));
            return v === null ? undefined
                : {value: v, writable: true, enumerable: true,
                   configurable: true};
        },
    });
}

const cache = new WeakMap();
function cached(element, name, make) {
    let entry = cache.get(element);
    if (!entry) cache.set(element, entry = {});
    return entry[name] || (entry[name] = make(element));
}

function byTag(scope, tag) {
    tag = String(tag).toLowerCase();
    return toArray(N.query(scope, tag === '*' ? '*' : tag, true));
}
function byClass(scope, names) {
    const list = String(names).trim().split(/\s+/).filter(Boolean);
    if (!list.length) return toArray([]);
    return toArray(N.query(scope, list.map(c => '.' + CSS.escape(c)).join(''),
                           true));
}

class Element extends Node {
    get tagName() { return this.nodeName; }
    get localName() { return N.name(this); }
    get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }
    get prefix() { return null; }
    get id() { return N.attr(this, 'id') || ''; }
    set id(v) { N.setAttr(this, 'id', String(v)); }
    get className() { return N.attr(this, 'class') || ''; }
    set className(v) { N.setAttr(this, 'class', String(v)); }
    get classList() {
        return cached(this, 'classList', e => new DOMTokenList(e, 'class'));
    }
    set classList(v) { N.setAttr(this, 'class', String(v)); }
    get relList() {
        return cached(this, 'relList', e => new DOMTokenList(e, 'rel'));
    }
    get attributes() {
        const list = N.attrNames(this).map(name =>
            ({name, localName: name, value: N.attr(this, name),
              nodeName: name, nodeValue: N.attr(this, name),
              specified: true}));
        for (const a of list) list[a.name] = a;
        define(list, 'getNamedItem', name => list[String(name).toLowerCase()]
                                             || null);
        return toArray(list);
    }
    getAttribute(name) { return N.attr(this, String(name)); }
    getAttributeNS(ns, name) { return N.attr(this, String(name)); }
    setAttribute(name, value) { N.setAttr(this, String(name), String(value)); }
    setAttributeNS(ns, name, value) {
        N.setAttr(this, String(name).replace(/^.*:/, ''), String(value));
    }
    removeAttribute(name) { N.removeAttr(this, String(name)); }
    removeAttributeNS(ns, name) { N.removeAttr(this, String(name)); }
    hasAttribute(name) { return N.attr(this, String(name)) !== null; }
    hasAttributes() { return N.attrNames(this).length > 0; }
    getAttributeNames() { return N.attrNames(this); }
    toggleAttribute(name, force) {
        const has = this.hasAttribute(name);
        const want = force === undefined ? !has : !!force;
        if (want && !has) N.setAttr(this, name, '');
        if (!want && has) N.removeAttr(this, name);
        return want;
    }
    get children() { return toArray(elementChildren(this)); }
    get childElementCount() { return elementChildren(this).length; }
    get firstElementChild() {
        for (let c = N.first(this); c; c = N.next(c))
            if (N.kind(c) === 1) return c;
        return null;
    }
    get lastElementChild() {
        for (let c = N.last(this); c; c = N.prev(c))
            if (N.kind(c) === 1) return c;
        return null;
    }
    get nextElementSibling() { return nextElement(this, N.next); }
    get previousElementSibling() { return nextElement(this, N.prev); }
    get innerHTML() { return N.html(this, false); }
    set innerHTML(markup) {
        for (const c of children(this)) N.remove(c);
        const fragment = N.fragment(String(markup ?? ''));
        N.inert(fragment);
        for (const c of children(fragment)) N.insert(this, c, null);
    }
    get outerHTML() { return N.html(this, true); }
    set outerHTML(markup) {
        const p = N.parent(this);
        if (!p) return;
        const fragment = N.fragment(String(markup ?? ''));
        N.inert(fragment);
        const next = N.next(this);
        N.remove(this);
        for (const c of children(fragment)) N.insert(p, c, next);
    }
    insertAdjacentHTML(where, markup) {
        const fragment = N.fragment(String(markup));
        N.inert(fragment);
        this._adjacent(where, children(fragment));
    }
    insertAdjacentElement(where, element) {
        this._adjacent(where, [element]);
        return element;
    }
    insertAdjacentText(where, text) {
        this._adjacent(where, [N.createText(String(text))]);
    }
    _adjacent(where, nodes) {
        where = String(where).toLowerCase();
        const p = N.parent(this);
        let parent = this, before = null;
        if (where === 'beforebegin') parent = p, before = this;
        else if (where === 'afterbegin') before = N.first(this);
        else if (where === 'afterend') parent = p, before = N.next(this);
        else if (where !== 'beforeend') throw new SyntaxError('bad position');
        if (!parent) return;
        for (const n of nodes) insert(parent, n, before);
    }
    append(...nodes) { for (const n of nodes) insert(this, toNode(n), null); }
    prepend(...nodes) {
        const first = N.first(this);
        for (const n of nodes) insert(this, toNode(n), first);
    }
    replaceChildren(...nodes) {
        for (const c of children(this)) N.remove(c);
        this.append(...nodes);
    }
    querySelector(s) { return N.query(this, String(s), false); }
    querySelectorAll(s) { return toArray(N.query(this, String(s), true)); }
    matches(s) { return N.matches(this, String(s)); }
    webkitMatchesSelector(s) { return this.matches(s); }
    closest(s) {
        for (let e = this; e; e = N.parent(e))
            if (N.kind(e) === 1 && N.matches(e, String(s))) return e;
        return null;
    }
    getElementsByTagName(t) { return byTag(this, t); }
    getElementsByClassName(c) { return byClass(this, c); }
    getBoundingClientRect() {
        return {x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0,
                height: 0, toJSON() { return this; }};
    }
    getClientRects() { return toArray([]); }
    scrollIntoView() {}
    scrollTo() {}
    scrollBy() {}
    get scrollTop() { return 0; }
    set scrollTop(v) {}
    get scrollLeft() { return 0; }
    set scrollLeft(v) {}
    get scrollWidth() { return 0; }
    get scrollHeight() { return 0; }
    get clientWidth() { return 0; }
    get clientHeight() { return 0; }
    get clientTop() { return 0; }
    get clientLeft() { return 0; }
    attachShadow() { throw new Error('NotSupportedError: shadow DOM'); }
    animate() {
        return {finished: Promise.resolve(), cancel() {}, play() {},
                pause() {}, onfinish: null};
    }
}
Object.assign(Element.prototype, childMixin);

const URL_ATTRIBUTES = ['href', 'src', 'action', 'formAction', 'poster',
                        'cite', 'data'];
const STRING_ATTRIBUTES = ['title', 'lang', 'dir', 'name', 'alt', 'rel',
    'target', 'placeholder', 'type', 'accept', 'autocomplete', 'role',
    'download', 'hreflang', 'media', 'sizes', 'srcset', 'integrity',
    'crossOrigin', 'referrerPolicy', 'loading', 'decoding', 'enctype',
    'method', 'htmlFor', 'accessKey', 'contentEditable', 'inputMode',
    'pattern', 'min', 'max', 'step', 'wrap', 'label', 'charset',
    'httpEquiv', 'scope', 'headers', 'shape', 'coords', 'useMap',
    'autocapitalize', 'enterKeyHint', 'nonce', 'as', 'colSpan', 'rowSpan',
    'span', 'start', 'width', 'height', 'maxLength', 'minLength', 'size',
    'cols', 'rows', 'tabIndex'];
const ATTRIBUTE_NAMES = {htmlFor: 'for', className: 'class',
                         httpEquiv: 'http-equiv', crossOrigin: 'crossorigin',
                         referrerPolicy: 'referrerpolicy',
                         formAction: 'formaction'};
const NUMERIC = new Set(['colSpan', 'rowSpan', 'span', 'start', 'width',
    'height', 'maxLength', 'minLength', 'size', 'cols', 'rows', 'tabIndex']);
const BOOLEAN_ATTRIBUTES = ['hidden', 'disabled', 'readOnly', 'required',
    'multiple', 'autofocus', 'async', 'defer', 'noModule', 'open', 'selected',
    'reversed', 'controls', 'autoplay', 'loop', 'muted', 'novalidate',
    'formNoValidate', 'allowFullscreen', 'isMap', 'inert', 'draggable',
    'spellcheck', 'translate', 'default', 'playsInline'];

class HTMLElement extends Element {
    get style() { return cached(this, 'style', styleFor); }
    set style(v) { N.setAttr(this, 'style', String(v)); }
    get dataset() { return cached(this, 'dataset', datasetFor); }
    get innerText() { return N.text(this); }
    set innerText(v) { this.textContent = v; }
    get outerText() { return N.text(this); }
    get offsetParent() { return N.parent(this); }
    get offsetTop() { return 0; }
    get offsetLeft() { return 0; }
    get offsetWidth() { return 0; }
    get offsetHeight() { return 0; }
    get isContentEditable() { return false; }
    click() {
        const event = new MouseEvent('click', {bubbles: true,
                                               cancelable: true});
        if (dispatch(this, event)) defaultAction(this, 'click');
    }
    focus() { document._active = this; }
    blur() { if (document._active === this) document._active = null; }
    get form() {
        for (let e = N.parent(this); e; e = N.parent(e))
            if (N.name(e) === 'form') return e;
        return null;
    }
    /* Form controls. */
    get value() {
        const tag = N.name(this);
        if (tag === 'input' || tag === 'textarea' || tag === 'select') {
            const v = N.field(this, 0);
            if (v !== undefined) return v;
        }
        if (tag === 'option') {
            const v = N.attr(this, 'value');
            return v === null ? N.text(this).trim() : v;
        }
        if (tag === 'progress' || tag === 'meter' || tag === 'li')
            return Number(N.attr(this, 'value') || 0);
        return N.attr(this, 'value') || '';
    }
    set value(v) {
        const tag = N.name(this);
        v = v === null || v === undefined ? '' : String(v);
        if (tag === 'select') {
            const options = this.options;
            const i = options.findIndex(o => o.value === v);
            N.field(this, 2, i);
            return;
        }
        if (tag === 'input' || tag === 'textarea') {
            if (N.field(this, 0) !== undefined) {
                N.field(this, 0, v);
                return;
            }
        }
        N.setAttr(this, 'value', v);
    }
    get defaultValue() {
        return N.name(this) === 'textarea' ? N.text(this)
            : N.attr(this, 'value') || '';
    }
    set defaultValue(v) { N.setAttr(this, 'value', String(v)); }
    get checked() {
        const v = N.field(this, 1);
        return v === undefined ? N.attr(this, 'checked') !== null : v;
    }
    set checked(v) {
        if (N.field(this, 1) !== undefined) N.field(this, 1, !!v);
        else if (v) N.setAttr(this, 'checked', '');
        else N.removeAttr(this, 'checked');
    }
    get defaultChecked() { return N.attr(this, 'checked') !== null; }
    get selectedIndex() {
        const v = N.field(this, 2);
        return v === undefined ? -1 : v;
    }
    set selectedIndex(i) { N.field(this, 2, i | 0); }
    get options() {
        return toArray(N.query(this, 'option', true));
    }
    get selectedOptions() {
        const i = this.selectedIndex;
        return toArray(i >= 0 && this.options[i] ? [this.options[i]] : []);
    }
    get length() {
        return N.name(this) === 'select' ? this.options.length
            : N.name(this) === 'form' ? this.elements.length : undefined;
    }
    get elements() {
        return toArray(N.query(this, 'input, select, textarea, button, '
                                     + 'fieldset, output, object', true));
    }
    get text() { return N.text(this); }
    set text(v) { this.textContent = v; }
    get labels() { return toArray([]); }
    get validity() { return {valid: true}; }
    checkValidity() { return true; }
    reportValidity() { return true; }
    setCustomValidity() {}
    select() {}
    setSelectionRange() {}
    submit() { defaultAction(this, 'submit'); }
    requestSubmit() {
        const event = new SubmitEvent('submit', {bubbles: true,
                                                 cancelable: true});
        if (dispatch(this, event)) defaultAction(this, 'submit');
    }
    reset() {}
    /* <a>, <area>: URL parts. */
    get protocol() { return new URL(this.href).protocol; }
    get host() { return new URL(this.href).host; }
    get hostname() { return new URL(this.href).hostname; }
    get port() { return new URL(this.href).port; }
    get pathname() { return new URL(this.href).pathname; }
    get search() { return new URL(this.href).search; }
    get hash() { return new URL(this.href).hash; }
    get origin() { return new URL(this.href).origin; }
    /* <img> */
    get complete() { return true; }
    get naturalWidth() { return 0; }
    get naturalHeight() { return 0; }
    get currentSrc() { return this.src; }
    decode() { return Promise.resolve(); }
    /* <template>; <meta content> */
    get content() {
        if (N.name(this) !== 'template') return N.attr(this, 'content') || '';
        const fragment = document.createDocumentFragment();
        for (const c of children(this)) N.insert(fragment, N.clone(c, true), null);
        return fragment;
    }
    /* <canvas>, <video>, <audio>: not drawn or played yet. */
    getContext() { return null; }
    toDataURL() { return 'data:,'; }
    play() { return Promise.reject(new Error('NotSupportedError')); }
    pause() {}
    load() {}
    canPlayType() { return ''; }
    /* <dialog>, <details> */
    showModal() { N.setAttr(this, 'open', ''); }
    show() { N.setAttr(this, 'open', ''); }
    close(v) { N.removeAttr(this, 'open'); this.returnValue = v || ''; }
}
for (const name of URL_ATTRIBUTES) {
    const attribute = ATTRIBUTE_NAMES[name] || name.toLowerCase();
    Object.defineProperty(HTMLElement.prototype, name, {
        get() {
            const v = N.attr(this, attribute);
            if (v === null) return '';
            return N.resolve(v.trim(), document.baseURI) || v;
        },
        set(v) { N.setAttr(this, attribute, String(v)); },
        configurable: true
    });
}
for (const name of STRING_ATTRIBUTES) {
    const attribute = ATTRIBUTE_NAMES[name] || name.toLowerCase();
    Object.defineProperty(HTMLElement.prototype, name, {
        get() {
            const v = N.attr(this, attribute);
            if (NUMERIC.has(name)) {
                const n = parseInt(v);
                return isNaN(n) ? (name === 'tabIndex' ? -1 : 0) : n;
            }
            if (name === 'type' && v === null)
                return N.name(this) === 'input' ? 'text'
                    : N.name(this) === 'button' ? 'submit' : '';
            if (name === 'method') return (v || 'get').toLowerCase();
            return v === null ? '' : v;
        },
        set(v) { N.setAttr(this, attribute, String(v)); },
        configurable: true
    });
}
for (const name of BOOLEAN_ATTRIBUTES) {
    const attribute = name.toLowerCase();
    if (name === 'selected') continue;
    Object.defineProperty(HTMLElement.prototype, name, {
        get() { return N.attr(this, attribute) !== null; },
        set(v) {
            if (v) N.setAttr(this, attribute, '');
            else N.removeAttr(this, attribute);
        },
        configurable: true
    });
}
Object.defineProperty(HTMLElement.prototype, 'selected', {
    get() {
        const select = this.closest('select');
        if (select) return select.options[select.selectedIndex] === this;
        return N.attr(this, 'selected') !== null;
    },
    set(v) {
        const select = this.closest('select');
        if (select && v) select.selectedIndex = select.options.indexOf(this);
    },
    configurable: true
});
addHandlerProperties(HTMLElement.prototype);

/* Element type names some scripts test with instanceof. */
const ELEMENT_TYPES = ['HTMLAnchorElement', 'HTMLAreaElement',
    'HTMLAudioElement', 'HTMLBodyElement', 'HTMLBRElement',
    'HTMLButtonElement', 'HTMLCanvasElement', 'HTMLDataElement',
    'HTMLDetailsElement', 'HTMLDialogElement', 'HTMLDivElement',
    'HTMLFieldSetElement', 'HTMLFormElement', 'HTMLHeadElement',
    'HTMLHeadingElement', 'HTMLHRElement', 'HTMLHtmlElement',
    'HTMLIFrameElement', 'HTMLImageElement', 'HTMLInputElement',
    'HTMLLabelElement', 'HTMLLIElement', 'HTMLLinkElement',
    'HTMLMediaElement', 'HTMLMetaElement', 'HTMLOListElement',
    'HTMLOptionElement', 'HTMLParagraphElement', 'HTMLPictureElement',
    'HTMLPreElement', 'HTMLScriptElement', 'HTMLSelectElement',
    'HTMLSlotElement', 'HTMLSourceElement', 'HTMLSpanElement',
    'HTMLStyleElement', 'HTMLTableCellElement', 'HTMLTableElement',
    'HTMLTableRowElement', 'HTMLTableSectionElement',
    'HTMLTemplateElement', 'HTMLTextAreaElement', 'HTMLTimeElement',
    'HTMLTitleElement', 'HTMLUListElement', 'HTMLUnknownElement',
    'HTMLVideoElement', 'SVGElement', 'SVGSVGElement'];
const TYPE_TAGS = {HTMLAnchorElement: ['a'], HTMLAreaElement: ['area'],
    HTMLAudioElement: ['audio'], HTMLBodyElement: ['body'],
    HTMLBRElement: ['br'], HTMLButtonElement: ['button'],
    HTMLCanvasElement: ['canvas'], HTMLDivElement: ['div'],
    HTMLFormElement: ['form'], HTMLHeadElement: ['head'],
    HTMLHeadingElement: ['h1', 'h2', 'h3', 'h4', 'h5', 'h6'],
    HTMLHtmlElement: ['html'], HTMLIFrameElement: ['iframe'],
    HTMLImageElement: ['img'], HTMLInputElement: ['input'],
    HTMLLabelElement: ['label'], HTMLLIElement: ['li'],
    HTMLLinkElement: ['link'], HTMLMediaElement: ['audio', 'video'],
    HTMLMetaElement: ['meta'], HTMLOListElement: ['ol'],
    HTMLOptionElement: ['option'], HTMLParagraphElement: ['p'],
    HTMLPreElement: ['pre'], HTMLScriptElement: ['script'],
    HTMLSelectElement: ['select'], HTMLSpanElement: ['span'],
    HTMLStyleElement: ['style'], HTMLTableCellElement: ['td', 'th'],
    HTMLTableElement: ['table'], HTMLTableRowElement: ['tr'],
    HTMLTableSectionElement: ['thead', 'tbody', 'tfoot'],
    HTMLTemplateElement: ['template'], HTMLTextAreaElement: ['textarea'],
    HTMLTitleElement: ['title'], HTMLUListElement: ['ul'],
    HTMLVideoElement: ['video'], SVGElement: null, SVGSVGElement: ['svg'],
    HTMLDetailsElement: ['details'], HTMLDialogElement: ['dialog'],
    HTMLFieldSetElement: ['fieldset'], HTMLPictureElement: ['picture'],
    HTMLSourceElement: ['source'], HTMLTimeElement: ['time'],
    HTMLDataElement: ['data'], HTMLSlotElement: ['slot']};
const SVG_TAGS = new Set(['svg', 'path', 'g', 'circle', 'rect', 'line',
    'polyline', 'polygon', 'ellipse', 'use', 'defs', 'symbol', 'text',
    'tspan', 'linearGradient', 'radialGradient', 'stop', 'clipPath', 'mask']);
for (const type of ELEMENT_TYPES) {
    const tags = TYPE_TAGS[type];
    const check = type === 'SVGElement' ? (n => SVG_TAGS.has(N.name(n)))
        : type === 'HTMLUnknownElement' ? (() => false)
        : tags ? (n => tags.includes(N.name(n))) : (() => false);
    const constructor = function () { illegal(); };
    Object.defineProperty(constructor, 'name', {value: type});
    constructor.prototype = HTMLElement.prototype;
    Object.defineProperty(constructor, Symbol.hasInstance, {
        value: v => v instanceof HTMLElement && check(v)
    });
    G[type] = constructor;
}

/* ------------------------------------------------------- the document */

class DocumentFragment extends Node {}
class Document extends EventTarget {
    constructor() { super(); define(this, '_active', null); }
    get nodeType() { return 9; }
    get nodeName() { return '#document'; }
    get documentElement() { return N.root(); }
    get head() { return N.head(); }
    get body() { return N.body(); }
    set body(v) {}
    get title() { return N.title(); }
    set title(v) { N.setTitle(String(v)); }
    get URL() { return N.url(); }
    get documentURI() { return N.url(); }
    get baseURI() {
        const base = N.query(N.root(), 'base[href]', false);
        return base ? N.resolve(N.attr(base, 'href'), N.url()) || N.url()
                    : N.url();
    }
    get location() { return location; }
    set location(v) { location.href = v; }
    get domain() { return location.hostname; }
    get referrer() { return ''; }
    get cookie() { return ''; }
    set cookie(v) {}
    get readyState() { return readyState; }
    get characterSet() { return 'UTF-8'; }
    get charset() { return 'UTF-8'; }
    get inputEncoding() { return 'UTF-8'; }
    get contentType() { return 'text/html'; }
    get compatMode() { return 'CSS1Compat'; }
    get doctype() { return null; }
    get defaultView() { return G; }
    get hidden() { return false; }
    get visibilityState() { return 'visible'; }
    get activeElement() { return this._active || N.body(); }
    get currentScript() { return N.currentScript(); }
    get scrollingElement() { return N.root(); }
    get forms() { return toArray(N.query(N.root(), 'form', true)); }
    get images() { return toArray(N.query(N.root(), 'img', true)); }
    get links() {
        return toArray(N.query(N.root(), 'a[href], area[href]', true));
    }
    get scripts() { return toArray(N.query(N.root(), 'script', true)); }
    get styleSheets() { return toArray([]); }
    get fonts() {
        return {ready: Promise.resolve(), add() {}, load() {
            return Promise.resolve([]); }, check() { return true; },
            addEventListener() {}, forEach() {}, status: 'loaded'};
    }
    get childNodes() { return toArray([N.root()]); }
    get children() { return toArray([N.root()]); }
    get firstChild() { return N.root(); }
    get lastChild() { return N.root(); }
    get firstElementChild() { return N.root(); }
    get parentNode() { return null; }
    get ownerDocument() { return null; }
    get implementation() {
        return {hasFeature: () => true,
                createHTMLDocument: () => document};
    }
    createElement(name) { return N.create(String(name)); }
    createElementNS(ns, name) {
        return N.create(String(name).replace(/^.*:/, ''));
    }
    createTextNode(text) { return N.createText(String(text)); }
    createComment() { return N.createText(''); }
    createDocumentFragment() { return N.create('#fragment'); }
    createEvent(kind) {
        kind = String(kind);
        if (/Mouse/.test(kind)) return new MouseEvent('');
        if (/Keyboard/.test(kind)) return new KeyboardEvent('');
        if (/Custom/.test(kind)) return new CustomEvent('');
        return new Event('');
    }
    createRange() {
        return {setStart() {}, setEnd() {}, selectNode() {},
                selectNodeContents() {}, collapse() {},
                createContextualFragment: markup => {
                    const f = N.fragment(String(markup));
                    return f;
                },
                getBoundingClientRect: () => ({top: 0, left: 0, width: 0,
                                               height: 0, right: 0,
                                               bottom: 0}),
                getClientRects: () => []};
    }
    createTreeWalker(root) {
        let current = root;
        return {root, get currentNode() { return current; },
                nextNode() {
                    let n = N.first(current);
                    while (!n && current && current !== root) {
                        n = N.next(current);
                        if (!n) current = N.parent(current);
                    }
                    current = n || current;
                    return n;
                }};
    }
    getElementById(id) { return N.byId(String(id)); }
    getElementsByTagName(t) {
        const tag = String(t).toLowerCase();
        const list = byTag(N.root(), tag);
        if (tag === 'html' || tag === '*') list.unshift(N.root());
        return list;
    }
    getElementsByClassName(c) { return byClass(N.root(), c); }
    getElementsByName(name) {
        return toArray(N.query(N.root(), '[name]', true)
            .filter(e => N.attr(e, 'name') === String(name)));
    }
    querySelector(s) {
        s = String(s);
        if (N.matches(N.root(), s)) return N.root();
        return N.query(N.root(), s, false);
    }
    querySelectorAll(s) {
        s = String(s);
        const list = N.query(N.root(), s, true);
        if (N.matches(N.root(), s)) list.unshift(N.root());
        return toArray(list);
    }
    contains(node) { return node === this || N.connected(node); }
    hasFocus() { return true; }
    write(...parts) {
        const markup = parts.join('');
        const script = N.currentScript();
        const fragment = N.fragment(markup);
        const nodes = children(fragment);
        if (script && N.parent(script)) {
            const next = N.next(script);
            for (const n of nodes) N.insert(N.parent(script), n, next);
        } else {
            for (const n of nodes) N.insert(N.body(), n, null);
        }
    }
    writeln(...parts) { this.write(...parts, '\n'); }
    open() { return this; }
    close() {}
    execCommand() { return false; }
    queryCommandSupported() { return false; }
    elementFromPoint() { return null; }
    elementsFromPoint() { return []; }
    getSelection() { return G.getSelection(); }
    exitFullscreen() { return Promise.resolve(); }
}
addHandlerProperties(Document.prototype);
let readyState = 'loading';
const document = Object.create(Document.prototype);
define(document, '_active', null);

/* --------------------------------------------------------------- URLs */

const URL_PATTERN = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?([^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
class URLSearchParams {
    constructor(init) {
        define(this, '_list', []);
        define(this, '_url', null);
        if (typeof init === 'string') {
            init = init.replace(/^\?/, '');
            for (const pair of init.split('&')) {
                if (!pair) continue;
                const eq = pair.indexOf('=');
                const k = eq < 0 ? pair : pair.slice(0, eq);
                const v = eq < 0 ? '' : pair.slice(eq + 1);
                this._list.push([decode(k), decode(v)]);
            }
        } else if (Array.isArray(init)) {
            for (const [k, v] of init) this._list.push([String(k), String(v)]);
        } else if (init && typeof init === 'object') {
            if (init instanceof URLSearchParams)
                for (const [k, v] of init._list) this._list.push([k, v]);
            else for (const k of Object.keys(init))
                this._list.push([k, String(init[k])]);
        }
    }
    _changed() { if (this._url) this._url._setSearch(this.toString()); }
    get size() { return this._list.length; }
    append(k, v) { this._list.push([String(k), String(v)]); this._changed(); }
    delete(k) {
        k = String(k);
        define(this, '_list', this._list.filter(p => p[0] !== k));
        this._changed();
    }
    get(k) {
        const p = this._list.find(p => p[0] === String(k));
        return p ? p[1] : null;
    }
    getAll(k) { return this._list.filter(p => p[0] === String(k)).map(p => p[1]); }
    has(k) { return this._list.some(p => p[0] === String(k)); }
    set(k, v) {
        k = String(k);
        const i = this._list.findIndex(p => p[0] === k);
        if (i < 0) this._list.push([k, String(v)]);
        else {
            this._list[i][1] = String(v);
            define(this, '_list', this._list.filter((p, j) => j <= i || p[0] !== k));
        }
        this._changed();
    }
    sort() { this._list.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0]); this._changed(); }
    forEach(fn, self) { for (const [k, v] of this._list) fn.call(self, v, k, this); }
    keys() { return this._list.map(p => p[0])[Symbol.iterator](); }
    values() { return this._list.map(p => p[1])[Symbol.iterator](); }
    entries() { return this._list.map(p => p.slice())[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() {
        return this._list.map(([k, v]) => encode(k) + '=' + encode(v)).join('&');
    }
}
function decode(s) {
    try { return decodeURIComponent(s.replace(/\+/g, ' ')); }
    catch (e) { return s; }
}
function encode(s) {
    return encodeURIComponent(s).replace(/%20/g, '+')
        .replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
}
class URL {
    constructor(url, base) {
        url = String(url);
        let absolute = url;
        if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(url) || base !== undefined) {
            const b = base === undefined ? undefined : String(base);
            if (b !== undefined && !URL_PATTERN.test(b))
                throw new TypeError('Invalid base URL: ' + b);
            const resolved = /^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(url) ? url
                : b !== undefined ? N.resolve(url, b) : null;
            if (!resolved) throw new TypeError('Invalid URL: ' + url);
            absolute = resolved;
        }
        const m = URL_PATTERN.exec(absolute);
        if (!m) throw new TypeError('Invalid URL: ' + url);
        define(this, '_p', {protocol: m[1].toLowerCase(), username: m[2] || '',
            password: m[3] || '', hostname: (m[4] || '').toLowerCase(),
            port: m[5] || '', pathname: m[6] || (m[4] !== undefined ? '/' : ''),
            search: m[7] && m[7] !== '?' ? m[7] : '',
            hash: m[8] && m[8] !== '#' ? m[8] : '',
            special: m[4] !== undefined});
        if ((this._p.protocol === 'http:' && this._p.port === '80')
            || (this._p.protocol === 'https:' && this._p.port === '443'))
            this._p.port = '';
        const params = new URLSearchParams(this._p.search);
        define(params, '_url', this);
        define(this, '_params', params);
    }
    _setSearch(s) { this._p.search = s ? '?' + s : ''; }
    get protocol() { return this._p.protocol; }
    set protocol(v) { this._p.protocol = String(v).replace(/:?$/, ':'); }
    get username() { return this._p.username; }
    get password() { return this._p.password; }
    get hostname() { return this._p.hostname; }
    set hostname(v) { this._p.hostname = String(v); }
    get port() { return this._p.port; }
    set port(v) { this._p.port = String(v); }
    get host() {
        return this._p.hostname + (this._p.port ? ':' + this._p.port : '');
    }
    set host(v) {
        const [h, p] = String(v).split(':');
        this._p.hostname = h;
        this._p.port = p || '';
    }
    get origin() {
        return this._p.special ? this._p.protocol + '//' + this.host : 'null';
    }
    get pathname() { return this._p.pathname; }
    set pathname(v) {
        v = String(v);
        this._p.pathname = v.startsWith('/') ? v : '/' + v;
    }
    get search() { return this._p.search; }
    set search(v) {
        v = String(v).replace(/^\?/, '');
        this._p.search = v ? '?' + v : '';
        const params = new URLSearchParams(v);
        define(params, '_url', this);
        define(this, '_params', params);
    }
    get searchParams() { return this._params; }
    get hash() { return this._p.hash; }
    set hash(v) {
        v = String(v).replace(/^#/, '');
        this._p.hash = v ? '#' + v : '';
    }
    get href() {
        const p = this._p;
        const auth = p.username ? p.username + (p.password ? ':' + p.password
                                                           : '') + '@' : '';
        return p.protocol + (p.special ? '//' + auth + this.host : '')
            + p.pathname + p.search + p.hash;
    }
    set href(v) {
        const u = new URL(v);
        define(this, '_p', u._p);
        define(this, '_params', u._params);
        define(this._params, '_url', this);
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static canParse(url, base) {
        try { new URL(url, base); return true; } catch (e) { return false; }
    }
    static createObjectURL() { return 'blob:null'; }
    static revokeObjectURL() {}
}

const location = Object.create(null);
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname',
                    'search', 'hash', 'origin']) {
    Object.defineProperty(location, part, {
        get() { return new URL(N.url())[part]; },
        set(v) {
            if (part === 'origin') return;
            const u = new URL(N.url());
            u[part] = v;
            N.navigate(u.href, false);
        },
        enumerable: true
    });
}
Object.defineProperty(location, 'href', {
    get() { return N.url(); },
    set(v) { N.navigate(String(v), false); },
    enumerable: true
});
Object.assign(location, {
    assign(url) { N.navigate(String(url), false); },
    replace(url) { N.navigate(String(url), true); },
    reload() { N.navigate(N.url(), true); },
    toString() { return N.url(); },
    ancestorOrigins: [],
});
Object.setPrototypeOf(location, Object.prototype);

/* --------------------------------------------------------- the window */

let timerId = 0;
const timerList = new Map();
function addTimer(fn, delay, args, repeat) {
    const id = ++timerId;
    delay = Math.max(0, Number(delay) || 0);
    timerList.set(id, {fn, args, delay: Math.max(repeat ? 4 : 0, delay),
                       at: N.now() + delay, repeat});
    return id;
}
function runTimers(now) {
    /* Timers due now, in order; those they add wait for the next round. */
    const due = [];
    for (const [id, t] of timerList) if (t.at <= now) due.push([id, t]);
    due.sort((a, b) => a[1].at - b[1].at || a[0] - b[0]);
    for (const [id, t] of due) {
        if (!timerList.has(id)) continue;
        if (t.repeat) t.at = Math.max(now, t.at + t.delay);
        else timerList.delete(id);
        try {
            if (typeof t.fn === 'function') t.fn.apply(G, t.args);
            else (0, eval)(String(t.fn));
        } catch (e) {
            reportError(e);
        }
    }
    let next = -1;
    const after = N.now();
    for (const t of timerList.values()) {
        const wait = Math.max(0, t.at - after);
        if (next < 0 || wait < next) next = wait;
    }
    return next;
}

class Storage {
    constructor() { define(this, '_m', new Map()); }
    get length() { return this._m.size; }
    key(i) { return Array.from(this._m.keys())[i] ?? null; }
    getItem(k) { k = String(k); return this._m.has(k) ? this._m.get(k) : null; }
    setItem(k, v) { this._m.set(String(k), String(v)); }
    removeItem(k) { this._m.delete(String(k)); }
    clear() { this._m.clear(); }
}

class MediaQueryList extends EventTarget {
    constructor(query) { super(); this.media = query; this.onchange = null; }
    get matches() { return N.media(this.media); }
    addListener(fn) { this.addEventListener('change', fn); }
    removeListener(fn) { this.removeEventListener('change', fn); }
}

class Window extends EventTarget {}
addHandlerProperties(Window.prototype);

class AbortSignal extends EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(reason) {
        const c = new AbortController();
        c.abort(reason);
        return c.signal;
    }
    static timeout(ms) {
        const c = new AbortController();
        addTimer(() => c.abort(new Error('TimeoutError')), ms, [], false);
        return c.signal;
    }
}
class AbortController {
    constructor() {
        const signal = Object.create(AbortSignal.prototype);
        signal.aborted = false;
        this.signal = signal;
    }
    abort(reason) {
        if (this.signal.aborted) return;
        this.signal.aborted = true;
        this.signal.reason = reason === undefined
            ? new Error('AbortError') : reason;
        dispatch(this.signal, new Event('abort'));
    }
}

class Observer {
    constructor(callback) { this._callback = callback; }
    observe() {}
    unobserve() {}
    disconnect() {}
    takeRecords() { return []; }
}
class MutationObserver extends Observer {}
class ResizeObserver extends Observer {}
class IntersectionObserver extends Observer {
    /* Reports everything as visible once, so lazy loaders load. */
    observe(target) {
        const callback = this._callback;
        const observer = this;
        addTimer(() => callback([{target, isIntersecting: true,
                                  intersectionRatio: 1, time: N.now(),
                                  boundingClientRect: target.getBoundingClientRect(),
                                  intersectionRect: target.getBoundingClientRect(),
                                  rootBounds: null}], observer), 0, [], false);
    }
}
class PerformanceObserver extends Observer {}
PerformanceObserver.supportedEntryTypes = [];

const performance = {
    now: () => N.now(),
    timeOrigin: Date.now() - N.now(),
    mark() {}, measure() {}, clearMarks() {}, clearMeasures() {},
    getEntries: () => [], getEntriesByType: () => [],
    getEntriesByName: () => [],
    timing: {navigationStart: Date.now() - N.now()},
    navigation: {type: 0},
};

const navigator = {
    userAgent: 'Mozilla/5.0 (OPOS; x86_64) OPOSBrowser/0.7',
    appName: 'Netscape', appVersion: '5.0 (OPOS)', appCodeName: 'Mozilla',
    product: 'Gecko', vendor: '', platform: 'OPOS', language: 'en-US',
    languages: ['en-US', 'en'], onLine: true, cookieEnabled: false,
    doNotTrack: '1', hardwareConcurrency: 1, maxTouchPoints: 0,
    webdriver: false, pdfViewerEnabled: false, plugins: [], mimeTypes: [],
    javaEnabled: () => false,
    sendBeacon: () => true,
    clipboard: {writeText: () => Promise.resolve(),
                readText: () => Promise.resolve('')},
    serviceWorker: undefined,
};

const screen = {
    get width() { return N.viewportWidth(); },
    get height() { return N.viewportHeight(); },
    get availWidth() { return N.viewportWidth(); },
    get availHeight() { return N.viewportHeight(); },
    colorDepth: 24, pixelDepth: 24,
    orientation: {type: 'landscape-primary', angle: 0,
                  addEventListener() {}},
};

const history = {
    get length() { return 1; },
    state: null,
    scrollRestoration: 'auto',
    back() {}, forward() {}, go() {},
    /* The address does not change yet; the state is kept. */
    pushState(state) { this.state = state; },
    replaceState(state) { this.state = state; },
};

const CSS = {
    supports: () => false,
    escape: s => String(s).replace(/([^\w-])/g, '\\$1')
                          .replace(/^(\d)/, '\\3$1 '),
};

function getComputedStyle(element) {
    const inline = element && element.style;
    return new Proxy({}, {
        get(t, key) {
            if (key === 'getPropertyValue')
                return name => inline ? inline.getPropertyValue(name) : '';
            if (typeof key !== 'string') return undefined;
            if (key === 'display') {
                const v = inline && inline.display;
                return v || (element && N.attr(element, 'hidden') !== null
                    ? 'none' : 'block');
            }
            if (key === 'visibility') return (inline && inline.visibility)
                                            || 'visible';
            return inline ? inline[key] : '';
        },
    });
}

/* Globals. */
const api = {
    window: G, self: G, top: G, parent: G, frames: G, opener: null,
    document, location, navigator, screen, history, performance, console,
    CSS, Node, Element, HTMLElement, CharacterData, Text, Comment,
    Document, HTMLDocument: Document, DocumentFragment, EventTarget, Event,
    CustomEvent, UIEvent, MouseEvent, PointerEvent, WheelEvent, FocusEvent,
    InputEvent, KeyboardEvent, SubmitEvent, ErrorEvent, ProgressEvent,
    PopStateEvent, HashChangeEvent, URL, URLSearchParams, DOMTokenList,
    Storage, AbortController, AbortSignal, MutationObserver,
    ResizeObserver, IntersectionObserver, PerformanceObserver,
    MediaQueryList, Window,
    localStorage: new Storage(), sessionStorage: new Storage(),
    name: '', closed: false, isSecureContext: true, origin: '',
    devicePixelRatio: 1, length: 0,
    get innerWidth() { return N.viewportWidth(); },
    get innerHeight() { return N.viewportHeight(); },
    get outerWidth() { return N.viewportWidth(); },
    get outerHeight() { return N.viewportHeight(); },
    scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0,
    screenX: 0, screenY: 0, screenLeft: 0, screenTop: 0,
    setTimeout: (fn, ms, ...args) => addTimer(fn, ms, args, false),
    setInterval: (fn, ms, ...args) => addTimer(fn, ms, args, true),
    clearTimeout: id => { timerList.delete(id); },
    clearInterval: id => { timerList.delete(id); },
    setImmediate: (fn, ...args) => addTimer(fn, 0, args, false),
    clearImmediate: id => { timerList.delete(id); },
    requestAnimationFrame: fn => addTimer(() => fn(N.now()), 16, [], false),
    cancelAnimationFrame: id => { timerList.delete(id); },
    requestIdleCallback: fn => addTimer(() => fn({didTimeout: false,
        timeRemaining: () => 10}), 1, [], false),
    cancelIdleCallback: id => { timerList.delete(id); },
    queueMicrotask: fn => { Promise.resolve().then(fn); },
    structuredClone: v => v === undefined ? v : JSON.parse(JSON.stringify(v)),
    reportError,
    getComputedStyle,
    matchMedia: query => new MediaQueryList(String(query)),
    alert: message => N.log('alert', String(message)),
    confirm: message => { N.log('alert', String(message)); return false; },
    prompt: message => { N.log('alert', String(message)); return null; },
    print() {}, focus() {}, blur() {}, stop() {}, close() {},
    open: url => { if (url) N.navigate(String(url), false); return null; },
    scroll() {}, scrollTo() {}, scrollBy() {}, moveTo() {}, resizeTo() {},
    postMessage() {},
    getSelection: () => ({rangeCount: 0, toString: () => '',
                          removeAllRanges() {}, addRange() {},
                          getRangeAt() { return document.createRange(); }}),
    btoa: text => {
        text = String(text);
        const table = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'
                    + '0123456789+/';
        let out = '';
        for (let i = 0; i < text.length; i += 3) {
            const a = text.charCodeAt(i), b = text.charCodeAt(i + 1),
                  c = text.charCodeAt(i + 2);
            if (a > 255 || b > 255 || c > 255)
                throw new Error('InvalidCharacterError');
            const n = a << 16 | (b || 0) << 8 | (c || 0);
            out += table[n >> 18 & 63] + table[n >> 12 & 63]
                + (i + 1 < text.length ? table[n >> 6 & 63] : '=')
                + (i + 2 < text.length ? table[n & 63] : '=');
        }
        return out;
    },
    atob: text => {
        const table = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'
                    + '0123456789+/';
        text = String(text).replace(/[\s=]/g, '');
        let out = '', bits = 0, value = 0;
        for (const ch of text) {
            const v = table.indexOf(ch);
            if (v < 0) throw new Error('InvalidCharacterError');
            value = value << 6 | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += String.fromCharCode(value >> bits & 255);
            }
        }
        return out;
    },
};
for (const key of Object.keys(api)) {
    const d = Object.getOwnPropertyDescriptor(api, key);
    d.configurable = true;
    if ('value' in d) d.writable = true;
    Object.defineProperty(G, key, d);
}
Object.setPrototypeOf(G, Window.prototype);

/* ------------------------------------------------- defaults and hooks */

/* What the browser does after an event nobody cancelled. */
function defaultAction(target, type) {
    if (type === 'submit') {
        N.log('info', 'form.submit() from a script is not supported yet');
        return;
    }
    for (let e = target; e; e = N.parent(e)) {
        if (N.kind(e) === 1 && N.name(e) === 'a'
            && N.attr(e, 'href') !== null) {
            const href = N.attr(e, 'href');
            if (/^\s*javascript:/i.test(href)) {
                try {
                    (0, eval)(decodeURIComponent(href.replace(/^\s*javascript:/i,
                                                              '')));
                } catch (x) {
                    reportError(x);
                }
            } else {
                N.navigate(href, false);
            }
            return;
        }
    }
}

return {
    elementProto: HTMLElement.prototype,
    textProto: Text.prototype,
    runTimers,
    /* A click (or other mouse event) from the browser; true when a script
       cancelled it. */
    dispatch(target, type, x, y, button) {
        const event = new MouseEvent(type, {bubbles: true, cancelable: true,
                                            clientX: x, clientY: y, button});
        Object.defineProperty(event, 'isTrusted', {value: true});
        if (type === 'click' && target instanceof HTMLElement)
            document._active = target;
        return !dispatch(target, event);
    },
    dispatchKey(target, type, key, keyCode, modifiers) {
        const event = new KeyboardEvent(type, {bubbles: true,
            cancelable: true, key, keyCode, code: key,
            shiftKey: !!(modifiers & 1), ctrlKey: !!(modifiers & 2),
            altKey: !!(modifiers & 4), metaKey: !!(modifiers & 8)});
        Object.defineProperty(event, 'isTrusted', {value: true});
        return !dispatch(target, event);
    },
    /* "load" on a <script src> once it ran. */
    fire(target, type) {
        dispatch(target, new Event(type));
    },
    fireGlobal(which, type) {
        if (type === 'DOMContentLoaded') {
            readyState = 'interactive';
            dispatch(document, new Event('readystatechange'));
        }
        if (type === 'load') {
            readyState = 'complete';
            dispatch(document, new Event('readystatechange'));
            dispatch(G, new Event('load'));
            dispatch(G, new Event('pageshow'));
            return;
        }
        dispatch(which === 'window' ? G : document,
                 new Event(type, {bubbles: true}));
    },
};
})
