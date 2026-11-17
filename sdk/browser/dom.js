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
    if (value instanceof Error)
        return value.name + ': ' + value.message
            + (value.stack && !depth
               ? '\n' + String(value.stack).split('\n').filter(Boolean)
                                           .slice(0, 4).join('\n') : '');
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
        for (const c of children(node)) {
            N.insert(parent, c, before || null);
            noteSheetLink(c);
        }
        return node;
    }
    N.insert(parent, node, before || null);
    noteSheetLink(node);
    return node;
}
/* A style sheet link a script added: the browser fetches it when it lays
   the page out (after this script), then "load" fires. */
function noteSheetLink(node) {
    if (N.kind(node) === 1 && N.name(node) === 'link'
        && /(^|\s)stylesheet(\s|$)/i.test(N.attr(node, 'rel') || '')
        && N.connected(node))
        addTimer(() => dispatch(node, new Event('load')), 0, [], false);
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
    return declaration(() => N.attr(element, 'style') || '', text => {
        if (text) N.setAttr(element, 'style', text);
        else N.removeAttr(element, 'style');
    });
}
/* A CSSStyleDeclaration kept as text: an element's style attribute or a
   rule's declarations. */
function declaration(getText, setText) {
    const read = () => parseStyle(getText());
    const write = map => {
        setText(Array.from(map, ([k, v]) => k + ': ' + v).join('; '));
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
            if (key === 'cssText') return getText();
            if (key === 'length') return read().size;
            if (/^\d+$/.test(key)) return target.item(+key);
            if (key === 'cssFloat') key = 'float';
            return read().get(KEBAB(key)) || '';
        },
        set(target, key, value) {
            if (typeof key !== 'string') return true;
            if (key === 'cssText') {
                setText(String(value));
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
    get innerHTML() {
        if (N.name(this) === 'template')
            return children(this.content).map(c => N.html(c, true)).join('');
        return N.html(this, false);
    }
    set innerHTML(markup) {
        const into = N.name(this) === 'template' ? this.content : this;
        for (const c of children(into)) N.remove(c);
        const fragment = N.fragment(String(markup ?? ''));
        N.inert(fragment);
        for (const c of children(fragment)) N.insert(into, c, null);
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
        const r = layoutRect(this);
        return r ? new DOMRect(r[0], r[1], r[2], r[3]) : new DOMRect();
    }
    getClientRects() {
        const r = layoutRect(this);
        return toArray(r ? [new DOMRect(r[0], r[1], r[2], r[3])] : []);
    }
    scrollIntoView(how) {
        const r = layoutRect(this);
        if (!r) return;
        const block = how === false ? 'end'
            : how && typeof how === 'object' ? how.block || 'start' : 'start';
        let y = N.metrics()[1] + r[1];
        if (block === 'center') y -= (N.viewportHeight() - r[3]) / 2;
        else if (block === 'end') y -= N.viewportHeight() - r[3];
        else if (block === 'nearest') {
            if (r[1] >= 0 && r[1] + r[3] <= N.viewportHeight()) return;
            if (r[1] > 0) y -= N.viewportHeight() - r[3];
        }
        N.scrollTo(y);
    }
    scrollTo(x, y) { if (isViewport(this)) G.scrollTo(x, y); }
    scrollBy(x, y) { if (isViewport(this)) G.scrollBy(x, y); }
    get scrollTop() { return isViewport(this) ? N.metrics()[1] : 0; }
    set scrollTop(v) { if (isViewport(this)) N.scrollTo(Number(v) || 0); }
    get scrollLeft() { return 0; }
    set scrollLeft(v) {}
    get scrollWidth() {
        if (isViewport(this)) return N.metrics()[2];
        const r = layoutRect(this);
        return r ? r[2] : 0;
    }
    get scrollHeight() {
        if (isViewport(this)) return N.metrics()[3];
        const r = layoutRect(this);
        return r ? r[3] : 0;
    }
    get clientWidth() {
        if (this === N.root()) return N.viewportWidth();
        const r = layoutRect(this);
        return r ? r[2] : 0;
    }
    get clientHeight() {
        if (this === N.root()) return N.viewportHeight();
        const r = layoutRect(this);
        return r ? r[3] : 0;
    }
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
    /* Only custom elements are made this way (new MyElement()). */
    constructor() { return constructCustom(new.target); }
    get style() { return cached(this, 'style', styleFor); }
    set style(v) { N.setAttr(this, 'style', String(v)); }
    get dataset() { return cached(this, 'dataset', datasetFor); }
    get innerText() { return N.text(this); }
    set innerText(v) { this.textContent = v; }
    get outerText() { return N.text(this); }
    /* Positions are given from the page's top left (the body), not from
       the nearest positioned box. */
    get offsetParent() {
        if (this === N.body() || this === N.root() || !layoutRect(this))
            return null;
        return N.body();
    }
    get offsetTop() {
        const r = layoutRect(this);
        return r ? r[1] + N.metrics()[1] : 0;
    }
    get offsetLeft() {
        const r = layoutRect(this);
        return r ? r[0] : 0;
    }
    get offsetWidth() {
        const r = layoutRect(this);
        return r ? r[2] : 0;
    }
    get offsetHeight() {
        const r = layoutRect(this);
        return r ? r[3] : 0;
    }
    get sheet() {
        const name = N.name(this);
        if (name === 'style') return cached(this, 'sheet', ownedSheet);
        if (name === 'link' && /(^|\s)stylesheet(\s|$)/i.test(
                N.attr(this, 'rel') || ''))
            return cached(this, 'sheet', ownedSheet);
        return null;
    }
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
    set defaultChecked(v) {
        if (v) N.setAttr(this, 'checked', '');
        else N.removeAttr(this, 'checked');
    }
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
        /* The children move to the fragment the first time. */
        return cached(this, 'content', element => {
            const fragment = N.create('#fragment');
            for (const c of children(element)) raw.insert(fragment, c, null);
            return fragment;
        });
    }
    set content(v) {
        if (N.name(this) !== 'template') N.setAttr(this, 'content', String(v));
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
G.HTMLScriptElement.supports = type =>
    ['classic', 'module', 'importmap'].includes(String(type));
/* new Image(), new Option(), new Audio(): elements made the old way. */
function Image(width, height) {
    const element = N.create('img');
    if (width !== undefined) N.setAttr(element, 'width', String(width));
    if (height !== undefined) N.setAttr(element, 'height', String(height));
    return element;
}
Image.prototype = G.HTMLImageElement.prototype;
function Option(text, value, defaultSelected, selected) {
    const element = N.create('option');
    if (text !== undefined) element.textContent = String(text);
    if (value !== undefined) N.setAttr(element, 'value', String(value));
    if (defaultSelected) N.setAttr(element, 'selected', '');
    if (selected) element.selected = true;
    return element;
}
Option.prototype = G.HTMLOptionElement.prototype;
function Audio(src) {
    const element = N.create('audio');
    if (src !== undefined) N.setAttr(element, 'src', String(src));
    return element;
}
Audio.prototype = G.HTMLAudioElement.prototype;

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
    get cookie() { return N.cookie(); }
    set cookie(v) { N.setCookie(String(v)); }
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
    get styleSheets() {
        const list = [];
        for (const e of N.query(N.root(), 'style, link', true)) {
            const sheet = e.sheet;
            if (sheet) list.push(sheet);
        }
        return toArray(list);
    }
    get adoptedStyleSheets() { return adopted.slice(); }
    set adoptedStyleSheets(list) {
        adopted = Array.from(list || []);
        flushAdopted();
    }
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
    createElement(name) { return createElement(String(name)); }
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
    /* Network requests waiting since the last round, then timers due now,
       in order; those they add wait for the next round. */
    for (const run of netQueue.splice(0)) {
        try {
            run();
        } catch (e) {
            reportError(e);
        }
    }
    now = N.now();
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
    flushStorage();
    if (resizeObservers.size > 0) checkResizes();
    if (netQueue.length > 0) return 0;
    let next = -1;
    const after = N.now();
    for (const t of timerList.values()) {
        const wait = Math.max(0, t.at - after);
        if (next < 0 || wait < next) next = wait;
    }
    return next;
}

class Storage {
    constructor() {
        define(this, '_m', new Map());
        define(this, '_changed', () => {});
    }
    get length() { return this._m.size; }
    key(i) { return Array.from(this._m.keys())[i] ?? null; }
    getItem(k) { k = String(k); return this._m.has(k) ? this._m.get(k) : null; }
    setItem(k, v) {
        k = String(k);
        v = String(v);
        let size = 0;
        for (const [key, value] of this._m)
            if (key !== k) size += key.length + value.length;
        if (size + k.length + v.length > 2500000)
            throw new DOMException('The quota has been exceeded.',
                                   'QuotaExceededError');
        this._m.set(k, v);
        this._changed();
    }
    removeItem(k) { if (this._m.delete(String(k))) this._changed(); }
    clear() { this._m.clear(); this._changed(); }
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
        addTimer(() => c.abort(new DOMException('signal timed out',
                                                'TimeoutError')), ms, [], false);
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
            ? new DOMException('signal is aborted without reason',
                               'AbortError') : reason;
        dispatch(this.signal, new Event('abort'));
    }
}

/* ------------------------------------------------------------ network */

class DOMException extends Error {
    constructor(message = '', name = 'Error') {
        super(message);
        this.name = name;
    }
}

class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s = '') { return new Uint8Array(N.encode(String(s))); }
}
class TextDecoder {
    constructor(label = 'utf-8') { this.encoding = String(label).toLowerCase(); }
    decode(data) {
        if (data === undefined) return '';
        return N.decode(toBuffer(data));
    }
}

/* Any body-like value as an ArrayBuffer of its own. */
function toBuffer(data) {
    if (data instanceof ArrayBuffer) return data.slice(0);
    if (ArrayBuffer.isView(data))
        return data.buffer.slice(data.byteOffset,
                                 data.byteOffset + data.byteLength);
    if (data instanceof Blob) return data._bytes.buffer.slice(0);
    return N.encode(String(data));
}

function joinBytes(parts) {
    let size = 0;
    for (const p of parts) size += p.byteLength;
    const out = new Uint8Array(size);
    let at = 0;
    for (const p of parts) { out.set(p, at); at += p.byteLength; }
    return out;
}

class Blob {
    constructor(parts = [], options = {}) {
        define(this, '_bytes', joinBytes(Array.from(parts,
            p => new Uint8Array(toBuffer(p)))));
        this.type = String(options.type || '').toLowerCase();
    }
    get size() { return this._bytes.byteLength; }
    text() { return Promise.resolve(N.decode(this._bytes.buffer.slice(0))); }
    arrayBuffer() { return Promise.resolve(this._bytes.buffer.slice(0)); }
    bytes() { return Promise.resolve(this._bytes.slice()); }
    slice(start = 0, end = this.size, type = '') {
        return new Blob([this._bytes.slice(start, end)], {type});
    }
}
class File extends Blob {
    constructor(parts, name, options = {}) {
        super(parts, options);
        this.name = String(name);
        this.lastModified = options.lastModified || Date.now();
    }
}

class FormData {
    constructor(form) {
        define(this, '_list', []);
        if (!form) return;
        for (const el of form.elements) {
            const name = el.getAttribute('name');
            const type = (el.getAttribute('type') || '').toLowerCase();
            if (!name || el.hasAttribute('disabled')
                || el.tagName === 'BUTTON' || el.tagName === 'FIELDSET'
                || el.tagName === 'OUTPUT' || el.tagName === 'OBJECT'
                || ['submit', 'button', 'reset', 'image', 'file']
                       .includes(type)
                || ((type === 'checkbox' || type === 'radio') && !el.checked))
                continue;
            this.append(name, type === 'checkbox' || type === 'radio'
                ? (el.getAttribute('value') ?? 'on') : el.value);
        }
    }
    append(name, value, filename) {
        this._list.push([String(name), value instanceof Blob
            ? (value instanceof File && filename === undefined ? value
               : new File([value], filename ?? 'blob', {type: value.type}))
            : String(value)]);
    }
    set(name, value, filename) {
        name = String(name);
        const i = this._list.findIndex(e => e[0] === name);
        this.delete(name);
        this.append(name, value, filename);
        if (i >= 0) this._list.splice(i, 0, this._list.pop());
    }
    get(name) {
        const e = this._list.find(e => e[0] === String(name));
        return e ? e[1] : null;
    }
    getAll(name) {
        return this._list.filter(e => e[0] === String(name)).map(e => e[1]);
    }
    has(name) { return this._list.some(e => e[0] === String(name)); }
    delete(name) {
        name = String(name);
        define(this, '_list', this._list.filter(e => e[0] !== name));
    }
    forEach(fn, self) {
        for (const [k, v] of this._list) fn.call(self, v, k, this);
    }
    *entries() { yield* this._list.map(e => [e[0], e[1]]); }
    *keys() { for (const e of this._list) yield e[0]; }
    *values() { for (const e of this._list) yield e[1]; }
    [Symbol.iterator]() { return this.entries(); }
}

class Headers {
    constructor(init) {
        define(this, '_m', new Map());
        if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
        else if (Array.isArray(init)) for (const [k, v] of init)
            this.append(k, v);
        else if (init) for (const k of Object.keys(init))
            this.append(k, init[k]);
    }
    append(k, v) {
        k = String(k).toLowerCase();
        v = String(v).trim();
        this._m.set(k, this._m.has(k) ? this._m.get(k) + ', ' + v : v);
    }
    set(k, v) { this._m.set(String(k).toLowerCase(), String(v).trim()); }
    get(k) { return this._m.get(String(k).toLowerCase()) ?? null; }
    has(k) { return this._m.has(String(k).toLowerCase()); }
    delete(k) { this._m.delete(String(k).toLowerCase()); }
    forEach(fn, self) {
        for (const [k, v] of this.entries()) fn.call(self, v, k, this);
    }
    getSetCookie() { return []; }
    *entries() {
        yield* Array.from(this._m.entries()).sort((a, b) =>
            a[0] < b[0] ? -1 : 1);
    }
    *keys() { for (const [k] of this.entries()) yield k; }
    *values() { for (const [, v] of this.entries()) yield v; }
    [Symbol.iterator]() { return this.entries(); }
}

function parseHeaders(text) {
    const h = new Headers();
    for (const line of String(text).split('\n')) {
        const i = line.indexOf(':');
        if (i > 0) h.append(line.slice(0, i).trim(), line.slice(i + 1).trim());
    }
    return h;
}

/* A request body as what N.http sends, and the Content-Type it implies. */
function encodeBody(body) {
    if (body === undefined || body === null) return [undefined, null];
    if (typeof body === 'string') return [body, 'text/plain;charset=UTF-8'];
    if (body instanceof URLSearchParams)
        return [body.toString(),
                'application/x-www-form-urlencoded;charset=UTF-8'];
    if (body instanceof FormData) {
        const boundary = '----OPOSFormBoundary'
            + Math.random().toString(36).slice(2);
        const parts = [];
        const text = s => new Uint8Array(N.encode(s));
        const quote = s => String(s).replace(/"/g, '%22')
                                    .replace(/\r?\n|\r/g, ' ');
        for (const [name, value] of body._list) {
            let head = '--' + boundary + '\r\nContent-Disposition: form-data; '
                + 'name="' + quote(name) + '"';
            if (value instanceof File)
                head += '; filename="' + quote(value.name) + '"\r\n'
                    + 'Content-Type: '
                    + (value.type || 'application/octet-stream');
            parts.push(text(head + '\r\n\r\n'));
            parts.push(value instanceof Blob ? value._bytes : text(value));
            parts.push(text('\r\n'));
        }
        parts.push(text('--' + boundary + '--\r\n'));
        return [joinBytes(parts).buffer,
                'multipart/form-data; boundary=' + boundary];
    }
    if (body instanceof Blob) return [toBuffer(body), body.type || null];
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body))
        return [toBuffer(body), null];
    return [String(body), 'text/plain;charset=UTF-8'];
}

const statusTexts = {200: 'OK', 201: 'Created', 202: 'Accepted',
    204: 'No Content', 206: 'Partial Content', 301: 'Moved Permanently',
    302: 'Found', 304: 'Not Modified', 400: 'Bad Request',
    401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found',
    405: 'Method Not Allowed', 409: 'Conflict', 429: 'Too Many Requests',
    500: 'Internal Server Error', 502: 'Bad Gateway',
    503: 'Service Unavailable'};

class Response {
    constructor(body = null, init = {}) {
        const [data, type] = encodeBody(body);
        define(this, '_buffer', data === undefined ? new ArrayBuffer(0)
                                                   : toBuffer(data));
        this.status = init.status ?? 200;
        this.statusText = init.statusText ?? (statusTexts[this.status] || '');
        this.headers = new Headers(init.headers);
        if (type && !this.headers.has('content-type'))
            this.headers.set('content-type', type);
        this.url = '';
        this.redirected = false;
        this.type = 'default';
        this.bodyUsed = false;
    }
    get ok() { return this.status >= 200 && this.status < 300; }
    get body() { return null; }
    _take() {
        if (this.bodyUsed)
            return Promise.reject(new TypeError('Body has already been read'));
        this.bodyUsed = true;
        return Promise.resolve(this._buffer);
    }
    arrayBuffer() { return this._take(); }
    bytes() { return this._take().then(b => new Uint8Array(b)); }
    text() { return this._take().then(b => N.decode(b)); }
    json() { return this.text().then(t => JSON.parse(t)); }
    blob() {
        return this._take().then(b => new Blob([b],
            {type: this.headers.get('content-type') || ''}));
    }
    formData() {
        return this.text().then(t => {
            const f = new FormData();
            for (const [k, v] of new URLSearchParams(t)) f.append(k, v);
            return f;
        });
    }
    clone() {
        const r = new Response(this._buffer.slice(0), {status: this.status,
            statusText: this.statusText, headers: this.headers});
        Object.assign(r, {url: this.url, redirected: this.redirected,
                          type: this.type});
        return r;
    }
    static json(data, init = {}) {
        const r = new Response(JSON.stringify(data), init);
        r.headers.set('content-type', 'application/json');
        return r;
    }
    static error() {
        const r = new Response(null, {status: 0});
        r.type = 'error';
        return r;
    }
    static redirect(url, status = 302) {
        return new Response(null, {status, headers: {location: url}});
    }
}

class Request {
    constructor(input, init = {}) {
        const from = input instanceof Request ? input : null;
        this.url = from ? from.url
            : N.resolve(String(input), null) || String(input);
        this.method = String(init.method || (from && from.method) || 'GET')
            .toUpperCase();
        this.headers = new Headers(init.headers || (from && from.headers));
        define(this, '_body', 'body' in init ? init.body
                                             : from ? from._body : null);
        this.credentials = init.credentials || (from && from.credentials)
            || 'same-origin';
        this.mode = init.mode || (from && from.mode) || 'cors';
        this.cache = init.cache || 'default';
        this.redirect = init.redirect || 'follow';
        this.referrer = 'about:client';
        this.signal = init.signal || (from && from.signal)
            || new AbortController().signal;
    }
    clone() { return new Request(this); }
    text() { return Promise.resolve(String(this._body ?? '')); }
    json() { return this.text().then(JSON.parse); }
}

/* Requests run one after another between timers and events (the page
   waits while one runs). */
const netQueue = [];

/* Runs one request: returns {status, url, headers, body} or throws. */
function httpNow(method, url, headers, body, credentials) {
    const [data, implied] = encodeBody(body);
    let type = headers.get('content-type');
    if (type === null && implied !== null) type = implied;
    const lines = [];
    headers.forEach((v, k) => { if (k !== 'content-type')
        lines.push(k + ': ' + v); });
    return N.http(method, url, lines.join('\n'), type, data,
                  credentials || 'same-origin');
}

function abortError() {
    return new DOMException('The operation was aborted.', 'AbortError');
}

function fetch(input, init = {}) {
    let request;
    try {
        request = new Request(input, init);
    } catch (e) {
        return Promise.reject(e);
    }
    return new Promise((resolve, reject) => {
        const signal = request.signal;
        if (signal.aborted) { reject(signal.reason || abortError()); return; }
        netQueue.push(() => {
            if (signal.aborted) { reject(signal.reason || abortError()); return; }
            let r;
            try {
                r = httpNow(request.method, request.url, request.headers,
                            request._body,
                            request.credentials === 'include'
                                ? 'include' : request.credentials);
            } catch (e) {
                reject(e instanceof TypeError ? e : new TypeError(String(e)));
                return;
            }
            const response = new Response(null, {status: r.status,
                headers: parseHeaders(r.headers)});
            define(response, '_buffer', r.body);
            response.url = r.url;
            response.redirected = r.url !== request.url.replace(/#.*/, '');
            response.type = 'basic';
            resolve(response);
        });
    });
}

class XMLHttpRequestEventTarget extends EventTarget {}
addHandlerProperties(XMLHttpRequestEventTarget.prototype);

class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
        super();
        this.readyState = 0;
        this.status = 0;
        this.statusText = '';
        this.responseType = '';
        this.responseURL = '';
        this.timeout = 0;
        this.withCredentials = false;
        this.upload = new XMLHttpRequestEventTarget();
        this.onreadystatechange = null;
        define(this, '_headers', new Headers());
        define(this, '_response', null);
        define(this, '_responseHeaders', new Headers());
        define(this, '_sent', 0);
    }
    _state(n) {
        this.readyState = n;
        dispatch(this, new Event('readystatechange'));
    }
    _progress(type, loaded) {
        dispatch(this, new ProgressEvent(type));
    }
    open(method, url, async = true) {
        define(this, '_method', String(method).toUpperCase());
        define(this, '_url', N.resolve(String(url), null) || String(url));
        define(this, '_async', async !== false);
        define(this, '_headers', new Headers());
        this._sent++;
        this.status = 0;
        this.statusText = '';
        this._response = null;
        this._state(1);
    }
    setRequestHeader(k, v) { this._headers.append(k, v); }
    overrideMimeType() {}
    getResponseHeader(k) { return this._responseHeaders.get(k); }
    getAllResponseHeaders() {
        let s = '';
        this._responseHeaders.forEach((v, k) => { s += k + ': ' + v + '\r\n'; });
        return s;
    }
    abort() {
        this._sent++;
        if (this.readyState === 1 || this.readyState === 3) {
            this._state(4);
            this._progress('abort');
            this._progress('loadend');
        }
        this.readyState = 0;
    }
    send(body = null) {
        if (this.readyState !== 1)
            throw new DOMException('The object is not open.',
                                   'InvalidStateError');
        const ticket = this._sent;
        const method = this._method;
        const run = () => {
            if (ticket !== this._sent) return; /* aborted or opened again */
            let r;
            try {
                r = httpNow(method, this._url, this._headers,
                            method === 'GET' || method === 'HEAD'
                                ? null : body,
                            this.withCredentials ? 'include' : 'same-origin');
            } catch (e) {
                this.readyState = 4;
                this.status = 0;
                dispatch(this, new Event('readystatechange'));
                this._progress('error');
                this._progress('loadend');
                return;
            }
            this.status = r.status;
            this.statusText = statusTexts[r.status] || '';
            this.responseURL = r.url;
            define(this, '_responseHeaders', parseHeaders(r.headers));
            define(this, '_body', r.body);
            this._state(2);
            this._state(3);
            this._state(4);
            this._progress('load');
            this._progress('loadend');
        };
        this._progress('loadstart');
        if (this._async) netQueue.push(run);
        else run();
    }
    get responseText() {
        if (this.responseType !== '' && this.responseType !== 'text')
            throw new DOMException('responseType is not text',
                                   'InvalidStateError');
        return this._body ? N.decode(this._body) : '';
    }
    get responseXML() { return null; }
    get response() {
        if (!this._body || this.readyState !== 4)
            return this.responseType === '' || this.responseType === 'text'
                ? '' : null;
        switch (this.responseType) {
        case 'arraybuffer': return this._body.slice(0);
        case 'blob': return new Blob([this._body],
            {type: this._responseHeaders.get('content-type') || ''});
        case 'json':
            try { return JSON.parse(N.decode(this._body)); }
            catch (e) { return null; }
        default: return N.decode(this._body);
        }
    }
}
Object.assign(XMLHttpRequest, {UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2,
                               LOADING: 3, DONE: 4});

/* localStorage is kept per site (saved after the timer round that
   changed it); sessionStorage only for the page. Both also answer
   storage.key = value and storage.key. */
function storageProxy(storage) {
    return new Proxy(storage, {
        get(t, k) {
            if (typeof k !== 'string' || k in t) {
                const v = Reflect.get(t, k, t);
                return typeof v === 'function' ? v.bind(t) : v;
            }
            return t.getItem(k);
        },
        set(t, k, v) {
            if (typeof k !== 'string' || k in t) return Reflect.set(t, k, v);
            t.setItem(k, v);
            return true;
        },
        has(t, k) { return k in t || t._m.has(k); },
        deleteProperty(t, k) { t.removeItem(k); return true; },
        ownKeys(t) { return Array.from(t._m.keys()); },
        getOwnPropertyDescriptor(t, k) {
            return t._m.has(k) ? {value: t._m.get(k), writable: true,
                                  enumerable: true, configurable: true}
                               : undefined;
        },
    });
}

let storageDirty = false;
const persistent = new Storage();
try {
    const saved = N.storageLoad();
    if (saved) for (const [k, v] of Object.entries(JSON.parse(saved)))
        persistent._m.set(k, String(v));
} catch (e) {
    /* a damaged file starts empty */
}
define(persistent, '_changed', () => { storageDirty = true; });
function flushStorage() {
    if (!storageDirty) return;
    storageDirty = false;
    N.storageSave(JSON.stringify(Object.fromEntries(persistent._m)));
}

/* ------------------------------------- mutations and custom elements */

/* Every change to the tree goes through N.insert, N.remove, N.setAttr,
   N.removeAttr and N.setData; while observers or custom elements exist,
   these record it. */
const raw = {insert: N.insert, remove: N.remove, setAttr: N.setAttr,
             removeAttr: N.removeAttr, setData: N.setData};
const registrations = [];          /* {observer, target, options} */
const customDefs = new Map();      /* name -> definition */
const customByClass = new Map();   /* constructor -> definition */
const upgraded = new WeakSet();
const definedWaits = new Map();    /* name -> [resolve] */
let upgrading = null;

function insideOf(ancestor, node) {
    if (ancestor === document) return N.connected(node);
    for (let n = node; n; n = N.parent(n)) if (n === ancestor) return true;
    return false;
}

class MutationRecord {
    constructor(type, target, info) {
        this.type = type;
        this.target = target;
        this.addedNodes = toArray(info.added || []);
        this.removedNodes = toArray(info.removed || []);
        this.previousSibling = info.previous || null;
        this.nextSibling = info.next || null;
        this.attributeName = info.name || null;
        this.attributeNamespace = null;
        this.oldValue = null;
    }
}

let deliveryQueued = false;
const observersWaiting = new Set();
function queueRecord(type, target, info) {
    const given = new Set();
    for (const reg of registrations) {
        const o = reg.options;
        if (given.has(reg.observer)) continue;
        if (reg.target !== target && !(o.subtree && insideOf(reg.target, target)))
            continue;
        if (type === 'childList' && !o.childList) continue;
        if (type === 'attributes' && (!o.attributes || (o.attributeFilter
                && !o.attributeFilter.includes(info.name)))) continue;
        if (type === 'characterData' && !o.characterData) continue;
        const record = new MutationRecord(type, target, info);
        if ((type === 'attributes' && o.attributeOldValue)
            || (type === 'characterData' && o.characterDataOldValue))
            record.oldValue = info.old;
        given.add(reg.observer);
        reg.observer._records.push(record);
        observersWaiting.add(reg.observer);
    }
    if (observersWaiting.size > 0 && !deliveryQueued) {
        deliveryQueued = true;
        Promise.resolve().then(deliverRecords);
    }
}
function deliverRecords() {
    deliveryQueued = false;
    const list = Array.from(observersWaiting);
    observersWaiting.clear();
    for (const observer of list) {
        const records = observer._records.splice(0);
        if (records.length === 0) continue;
        try {
            observer._callback.call(observer, records, observer);
        } catch (e) {
            reportError(e);
        }
    }
}

function customCall(element, name, args) {
    const f = element[name];
    if (typeof f !== 'function') return;
    try { f.apply(element, args); } catch (e) { reportError(e); }
}
function upgrade(element, def) {
    if (upgraded.has(element)) return;
    upgraded.add(element);
    upgrading = element;
    try {
        new def.ctor();
    } catch (e) {
        reportError(e);
    }
    upgrading = null;
    for (const name of def.observed) {
        const value = N.attr(element, name);
        if (value !== null)
            customCall(element, 'attributeChangedCallback',
                       [name, null, value]);
    }
    if (N.connected(element)) customCall(element, 'connectedCallback', []);
}
/* The custom elements in node's tree (node first). */
function customIn(node) {
    if (N.kind(node) !== 1 && !isFragment(node)) return [];
    const list = N.kind(node) === 1 ? [node] : [];
    for (const e of N.query(node, '*', true)) list.push(e);
    return list.filter(e => N.name(e).includes('-'));
}
function customConnected(node) {
    for (const e of customIn(node)) {
        const def = customDefs.get(N.name(e));
        if (!def) continue;
        if (!upgraded.has(e)) upgrade(e, def);
        else customCall(e, 'connectedCallback', []);
    }
}
function customDisconnected(node) {
    for (const e of customIn(node))
        if (upgraded.has(e)) customCall(e, 'disconnectedCallback', []);
}
function attributeChanged(element, name, old, value) {
    if (!upgraded.has(element)) return;
    const def = customDefs.get(N.name(element));
    if (def && def.observed.includes(name))
        customCall(element, 'attributeChangedCallback', [name, old, value]);
}

const watching = () => registrations.length > 0 || customDefs.size > 0;
N.insert = (parent, node, before) => {
    if (!watching()) return raw.insert(parent, node, before);
    const old = N.parent(node);
    const was = old !== null && N.connected(node);
    if (old !== null) {
        queueRecord('childList', old, {removed: [node], previous: N.prev(node),
                                       next: N.next(node)});
        if (was) customDisconnected(node);
    }
    const result = raw.insert(parent, node, before);
    queueRecord('childList', parent, {added: [node], previous: N.prev(node),
                                      next: N.next(node)});
    if (customDefs.size > 0 && N.connected(node)) customConnected(node);
    return result;
};
N.remove = node => {
    if (!watching()) return raw.remove(node);
    const parent = N.parent(node);
    const was = parent !== null && N.connected(node);
    if (parent !== null)
        queueRecord('childList', parent, {removed: [node],
                                          previous: N.prev(node),
                                          next: N.next(node)});
    const result = raw.remove(node);
    if (was && customDefs.size > 0) customDisconnected(node);
    return result;
};
N.setAttr = (element, name, value) => {
    if (!watching()) return raw.setAttr(element, name, value);
    const key = String(name).toLowerCase();
    const old = N.attr(element, name);
    const result = raw.setAttr(element, name, value);
    queueRecord('attributes', element, {name: key, old});
    attributeChanged(element, key, old, String(value));
    return result;
};
N.removeAttr = (element, name) => {
    if (!watching()) return raw.removeAttr(element, name);
    const key = String(name).toLowerCase();
    const old = N.attr(element, name);
    const result = raw.removeAttr(element, name);
    if (old !== null) {
        queueRecord('attributes', element, {name: key, old});
        attributeChanged(element, key, old, null);
    }
    return result;
};
N.setData = (node, value) => {
    if (!watching()) return raw.setData(node, value);
    const old = N.data(node);
    const result = raw.setData(node, value);
    queueRecord('characterData', node, {old});
    return result;
};

class MutationObserver {
    constructor(callback) {
        if (typeof callback !== 'function')
            throw new TypeError('MutationObserver needs a function');
        define(this, '_callback', callback);
        define(this, '_records', []);
    }
    observe(target, options = {}) {
        if (!(target instanceof Node) && target !== document)
            throw new TypeError('observe: not a Node');
        const o = Object.assign({}, options);
        if (o.attributeFilter !== undefined) {
            o.attributeFilter = Array.from(o.attributeFilter, String);
            if (o.attributes === undefined) o.attributes = true;
        }
        if (o.attributeOldValue && o.attributes === undefined)
            o.attributes = true;
        if (o.characterDataOldValue && o.characterData === undefined)
            o.characterData = true;
        if (!o.childList && !o.attributes && !o.characterData)
            throw new TypeError('observe: nothing to observe');
        const old = registrations.find(r => r.observer === this
                                            && r.target === target);
        if (old) old.options = o;
        else registrations.push({observer: this, target, options: o});
    }
    disconnect() {
        for (let i = registrations.length - 1; i >= 0; i--)
            if (registrations[i].observer === this) registrations.splice(i, 1);
        this._records.length = 0;
    }
    takeRecords() { return this._records.splice(0); }
}

const customElements = {
    define(name, ctor, options) {
        name = String(name);
        if (!/^[a-z][a-z0-9._·-￿]*-[a-z0-9._·-￿-]*$/
                .test(name))
            throw new DOMException('"' + name + '" is not a valid custom '
                                   + 'element name', 'SyntaxError');
        if (customDefs.has(name) || customByClass.has(ctor))
            throw new DOMException('"' + name + '" is already defined',
                                   'NotSupportedError');
        if (typeof ctor !== 'function')
            throw new TypeError('define: not a constructor');
        const def = {name, ctor, observed:
            Array.from(ctor.observedAttributes || [], a => String(a)
                       .toLowerCase()),
            extends: options && options.extends};
        customDefs.set(name, def);
        customByClass.set(ctor, def);
        for (const e of N.query(N.root(), name, true)) upgrade(e, def);
        for (const resolve of definedWaits.get(name) || []) resolve(ctor);
        definedWaits.delete(name);
    },
    get(name) {
        const def = customDefs.get(String(name));
        return def ? def.ctor : undefined;
    },
    getName(ctor) {
        const def = customByClass.get(ctor);
        return def ? def.name : null;
    },
    whenDefined(name) {
        name = String(name);
        const def = customDefs.get(name);
        if (def) return Promise.resolve(def.ctor);
        return new Promise(resolve => {
            if (!definedWaits.has(name)) definedWaits.set(name, []);
            definedWaits.get(name).push(resolve);
        });
    },
    upgrade(root) {
        for (const e of customIn(root)) {
            const def = customDefs.get(N.name(e));
            if (def) upgrade(e, def);
        }
    },
};
/* new MyElement() and upgrades both come here (HTMLElement's
   constructor). */
function constructCustom(target) {
    const def = customByClass.get(target);
    if (!def) illegal();
    let element = upgrading;
    upgrading = null;
    if (!element) {
        element = N.create(def.name);
        upgraded.add(element);
    }
    Object.setPrototypeOf(element, target.prototype);
    return element;
}
function createElement(name) {
    const element = N.create(name);
    const def = customDefs.get(String(name).toLowerCase());
    if (def) upgrade(element, def);
    return element;
}

/* ResizeObserver: sizes from the layout, checked after each timer round
   and when the window changes size. */
const resizeObservers = new Set();
class ResizeObserver {
    constructor(callback) {
        if (typeof callback !== 'function')
            throw new TypeError('ResizeObserver needs a function');
        define(this, '_callback', callback);
        define(this, '_targets', new Map());
    }
    observe(target) {
        if (!this._targets.has(target)) this._targets.set(target, [0, 0]);
        resizeObservers.add(this);
        addTimer(checkResizes, 0, [], false);
    }
    unobserve(target) {
        this._targets.delete(target);
        if (this._targets.size === 0) resizeObservers.delete(this);
    }
    disconnect() {
        this._targets.clear();
        resizeObservers.delete(this);
    }
}
function checkResizes() {
    for (const observer of Array.from(resizeObservers)) {
        const entries = [];
        for (const [target, last] of observer._targets) {
            const r = layoutRect(target);
            const w = r ? r[2] : 0, h = r ? r[3] : 0;
            if (w === last[0] && h === last[1]) continue;
            observer._targets.set(target, [w, h]);
            const size = [{inlineSize: w, blockSize: h}];
            entries.push({target, contentRect: new DOMRectReadOnly(0, 0, w, h),
                          borderBoxSize: size, contentBoxSize: size,
                          devicePixelContentBoxSize: size});
        }
        if (entries.length === 0) continue;
        try {
            observer._callback.call(observer, entries, observer);
        } catch (e) {
            reportError(e);
        }
    }
}

class Observer {
    constructor(callback) { this._callback = callback; }
    observe() {}
    unobserve() {}
    disconnect() {}
    takeRecords() { return []; }
}
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

/* Random numbers from the system; no crypto.subtle yet. */
class Crypto {
    getRandomValues(array) {
        if (!ArrayBuffer.isView(array) || array instanceof Float32Array
            || array instanceof Float64Array || array instanceof DataView)
            throw new DOMException('Not an integer array',
                                   'TypeMismatchError');
        if (array.byteLength > 65536)
            throw new DOMException('More than 65536 bytes',
                                   'QuotaExceededError');
        new Uint8Array(array.buffer, array.byteOffset, array.byteLength)
            .set(new Uint8Array(N.random(array.byteLength)));
        return array;
    }
    randomUUID() {
        const b = this.getRandomValues(new Uint8Array(16));
        b[6] = b[6] & 15 | 64;
        b[8] = b[8] & 63 | 128;
        const h = Array.from(b, x => (x + 256).toString(16).slice(1)).join('');
        return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16)
            + '-' + h.slice(16, 20) + '-' + h.slice(20);
    }
}
const crypto = new Crypto();

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
    languages: ['en-US', 'en'], onLine: true, cookieEnabled: true,
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

/* pushState changes the address without loading it; going back loads the
   earlier address again (there is no popstate). */
let historyLength = 1;
const history = {
    get length() { return historyLength; },
    state: null,
    scrollRestoration: 'auto',
    back() { N.navigateHistory(-1); },
    forward() { N.navigateHistory(1); },
    go(n) { if (n) N.navigateHistory(Number(n) || 0); },
    pushState(state, title, url) {
        if (url !== undefined && url !== null) N.pushUrl(String(url), false);
        historyLength++;
        this.state = state === undefined ? null : structuredCopy(state);
    },
    replaceState(state, title, url) {
        if (url !== undefined && url !== null) N.pushUrl(String(url), true);
        this.state = state === undefined ? null : structuredCopy(state);
    },
};
function structuredCopy(v) {
    try { return JSON.parse(JSON.stringify(v)); } catch (e) { return v; }
}

const CSS = {
    supports: () => false,
    escape: s => String(s).replace(/([^\w-])/g, '\\$1')
                          .replace(/^(\d)/, '\\3$1 '),
};

/* ------------------------------------------------- layout and styles */

class DOMRectReadOnly {
    constructor(x = 0, y = 0, width = 0, height = 0) {
        this.x = +x; this.y = +y; this.width = +width; this.height = +height;
    }
    get top() { return Math.min(this.y, this.y + this.height); }
    get bottom() { return Math.max(this.y, this.y + this.height); }
    get left() { return Math.min(this.x, this.x + this.width); }
    get right() { return Math.max(this.x, this.x + this.width); }
    toJSON() {
        const {x, y, width, height, top, right, bottom, left} = this;
        return {x, y, width, height, top, right, bottom, left};
    }
    static fromRect(r = {}) {
        return new this(r.x, r.y, r.width, r.height);
    }
}
class DOMRect extends DOMRectReadOnly {}

/* The element's box in the view after laying out what changed, or null
   when it shows nothing. */
function layoutRect(element) {
    flushSheets();
    return N.rect(element);
}
function isViewport(element) {
    return element === N.root() || element === N.body();
}

/* Style sheet text split into its top-level rules. */
function splitRules(text) {
    text = String(text).replace(/\/\*[\s\S]*?\*\//g, '');
    const rules = [];
    let depth = 0, start = 0, quote = '';
    const push = end => {
        const rule = text.slice(start, end).trim();
        if (rule && rule !== ';') rules.push(rule);
        start = end;
    };
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (quote) {
            if (c === '\\') i++;
            else if (c === quote) quote = '';
        } else if (c === '"' || c === "'") {
            quote = c;
        } else if (c === '{') {
            depth++;
        } else if (c === '}') {
            if (depth > 0 && --depth === 0) push(i + 1);
        } else if (c === ';' && depth === 0) {
            push(i + 1);
        }
    }
    return rules;
}

const GROUP_RULES = {media: 4, supports: 12, layer: 18, container: 0,
                     document: 13, '-moz-document': 13, scope: 0};
class CSSRule {
    constructor(parentRule, sheet) {
        define(this, '_parent', parentRule || null);
        define(this, '_sheet', sheet || null);
    }
    get parentRule() { return this._parent; }
    get parentStyleSheet() {
        return this._sheet || (this._parent && this._parent.parentStyleSheet);
    }
    _changed() {
        const sheet = this.parentStyleSheet;
        if (sheet) sheet._commit();
    }
}
Object.assign(CSSRule, {STYLE_RULE: 1, CHARSET_RULE: 2, IMPORT_RULE: 3,
    MEDIA_RULE: 4, FONT_FACE_RULE: 5, PAGE_RULE: 6, KEYFRAMES_RULE: 7,
    KEYFRAME_RULE: 8, NAMESPACE_RULE: 10, SUPPORTS_RULE: 12});
class CSSStyleRule extends CSSRule {
    constructor(selector, body, parentRule, sheet) {
        super(parentRule, sheet);
        define(this, '_selector', selector);
        define(this, '_body', body);
    }
    get type() { return 1; }
    get selectorText() { return this._selector; }
    set selectorText(v) { this._selector = String(v); this._changed(); }
    get style() {
        return cached(this, 'style', () => declaration(() => this._body,
            text => { this._body = text; this._changed(); }));
    }
    get cssText() {
        return this._selector + ' { ' + this._body
            + (this._body && !this._body.endsWith(';') ? '; }' : ' }');
    }
}
class CSSGroupingRule extends CSSRule {
    constructor(keyword, prelude, body, parentRule, sheet) {
        super(parentRule, sheet);
        define(this, '_keyword', keyword);
        define(this, '_prelude', prelude);
        define(this, '_rules', []);
        for (const t of splitRules(body)) {
            try { this._rules.push(makeRule(t, this, null)); } catch (e) {}
        }
    }
    get type() { return GROUP_RULES[this._keyword] || 0; }
    get cssRules() { return toArray(this._rules.slice()); }
    get conditionText() { return this._prelude.replace(/^@[\w-]+\s*/, ''); }
    get media() {
        const text = this.conditionText;
        const list = toArray(text.split(',').map(s => s.trim())
                                 .filter(Boolean));
        define(list, 'mediaText', text);
        return list;
    }
    get name() { return this.conditionText; }
    insertRule(text, index = 0) {
        index = Number(index) >>> 0;
        if (index > this._rules.length)
            throw new DOMException('Index out of range', 'IndexSizeError');
        this._rules.splice(index, 0, makeRule(String(text), this, null));
        this._changed();
        return index;
    }
    deleteRule(index) {
        index = Number(index) >>> 0;
        if (index >= this._rules.length)
            throw new DOMException('Index out of range', 'IndexSizeError');
        this._rules.splice(index, 1);
        this._changed();
    }
    get cssText() {
        return this._prelude + ' {\n'
            + this._rules.map(r => '  ' + r.cssText).join('\n') + '\n}';
    }
}
class CSSOtherRule extends CSSRule {
    constructor(keyword, text, parentRule, sheet) {
        super(parentRule, sheet);
        define(this, '_keyword', keyword);
        define(this, '_text', text);
    }
    get type() {
        return {'font-face': 5, keyframes: 7, '-webkit-keyframes': 7,
                import: 3, charset: 2, page: 6, namespace: 10}[this._keyword]
            || 0;
    }
    get name() {
        const m = /^@[\w-]+\s+([^{\s]+)/.exec(this._text);
        return m ? m[1] : '';
    }
    get href() {
        const m = /url\(\s*['"]?([^'")]*)|['"]([^'"]*)/.exec(this._text);
        return m ? m[1] || m[2] || '' : '';
    }
    get style() {
        const open = this._text.indexOf('{');
        const body = open < 0 ? '' : this._text.slice(open + 1,
                                                       this._text.lastIndexOf('}'));
        return declaration(() => body.trim(), () => {});
    }
    get cssText() { return this._text; }
}
function makeRule(text, parentRule, sheet) {
    text = String(text).trim();
    const syntax = () => new DOMException('Failed to parse the rule \''
                                          + text + '\'', 'SyntaxError');
    if (text[0] !== '@') {
        const open = text.indexOf('{');
        const close = text.lastIndexOf('}');
        if (open <= 0 || close < open) throw syntax();
        return new CSSStyleRule(text.slice(0, open).trim(),
                                text.slice(open + 1, close).trim(),
                                parentRule, sheet);
    }
    const m = /^@([\w-]+)/.exec(text);
    if (!m) throw syntax();
    const keyword = m[1].toLowerCase();
    const open = text.indexOf('{');
    if (keyword in GROUP_RULES && open > 0) {
        return new CSSGroupingRule(keyword, text.slice(0, open).trim(),
                                   text.slice(open + 1, text.lastIndexOf('}')),
                                   parentRule, sheet);
    }
    return new CSSOtherRule(keyword, text, parentRule, sheet);
}

/* Sheets whose rules changed; their text goes to the browser before the
   next layout. */
const sheetsChanged = new Set();
let adopted = [];
function flushSheets() {
    if (sheetsChanged.size === 0) return;
    const list = Array.from(sheetsChanged);
    sheetsChanged.clear();
    let adoptedChanged = false;
    for (const sheet of list) {
        const owner = sheet._owner;
        if (owner && N.name(owner) === 'style')
            N.setSheet(owner, sheet._disabled ? '' : sheet._text());
        if (adopted.includes(sheet)) adoptedChanged = true;
    }
    if (adoptedChanged) flushAdopted();
}
function flushAdopted() {
    const text = adopted.filter(s => !s._disabled).map(s => s._text())
                        .join('\n');
    N.setSheet(null, text || null);
}

class StyleSheet {}
class CSSStyleSheet extends StyleSheet {
    constructor(options) {
        super();
        define(this, '_owner', null);
        define(this, '_rules', []);
        define(this, '_source', null);   /* the owner's text parsed */
        define(this, '_edited', false);
        define(this, '_disabled', Boolean(options && options.disabled));
        define(this, '_media', options && options.media
            ? String(options.media) : '');
    }
    get type() { return 'text/css'; }
    get ownerNode() { return this._owner; }
    get ownerRule() { return null; }
    get parentStyleSheet() { return null; }
    get title() {
        return this._owner ? N.attr(this._owner, 'title') : null;
    }
    get href() {
        return this._owner && N.name(this._owner) === 'link'
            ? N.resolve(N.attr(this._owner, 'href') || '', N.url()) : null;
    }
    get media() {
        const text = this._owner ? N.attr(this._owner, 'media') || ''
                                 : this._media;
        const list = toArray(text.split(',').map(s => s.trim())
                                 .filter(Boolean));
        define(list, 'mediaText', text);
        return list;
    }
    get disabled() {
        if (this._owner && N.name(this._owner) === 'link')
            return N.attr(this._owner, 'disabled') !== null;
        return this._disabled;
    }
    set disabled(v) {
        v = Boolean(v);
        if (this._owner && N.name(this._owner) === 'link') {
            if (v) N.setAttr(this._owner, 'disabled', '');
            else N.removeAttr(this._owner, 'disabled');
            return;
        }
        if (v === this._disabled) return;
        this._disabled = v;
        this._sync();
        this._commit(true);
    }
    /* Reads the owner <style>'s text again when it changed. */
    _sync() {
        const owner = this._owner;
        if (!owner || N.name(owner) !== 'style') return;
        const text = N.text(owner);
        if (text === this._source) return;
        this._source = text;
        this._edited = false;
        this._rules = [];
        for (const t of splitRules(text)) {
            try { this._rules.push(makeRule(t, null, this)); } catch (e) {}
        }
    }
    _checkRules() {
        if (this._owner && N.name(this._owner) === 'link') {
            const href = this.href || '';
            if (!href.startsWith(location.origin + '/'))
                throw new DOMException('Cannot access rules',
                                       'SecurityError');
        }
        this._sync();
    }
    get cssRules() {
        this._checkRules();
        return toArray(this._rules.slice());
    }
    get rules() { return this.cssRules; }
    _text() { return this._rules.map(r => r.cssText).join('\n'); }
    _commit(force) {
        if (this._owner && N.name(this._owner) === 'link') return;
        this._edited = true;
        if (sheetsChanged.size === 0) Promise.resolve().then(flushSheets);
        sheetsChanged.add(this);
    }
    insertRule(text, index = 0) {
        this._checkRules();
        index = Number(index) >>> 0;
        if (index > this._rules.length)
            throw new DOMException('Index out of range', 'IndexSizeError');
        this._rules.splice(index, 0, makeRule(String(text), null, this));
        this._commit();
        return index;
    }
    deleteRule(index) {
        this._checkRules();
        index = Number(index) >>> 0;
        if (index >= this._rules.length)
            throw new DOMException('Index out of range', 'IndexSizeError');
        this._rules.splice(index, 1);
        this._commit();
    }
    addRule(selector = 'undefined', style = 'undefined', index) {
        this.insertRule(selector + ' { ' + style + ' }',
                        index === undefined ? this.cssRules.length : index);
        return -1;
    }
    removeRule(index = 0) { this.deleteRule(index); }
    replaceSync(text) {
        if (this._owner)
            throw new DOMException('Not a constructed sheet',
                                   'NotAllowedError');
        this._rules = [];
        for (const t of splitRules(text)) {
            try { this._rules.push(makeRule(t, null, this)); } catch (e) {}
        }
        this._commit();
    }
    replace(text) {
        try { this.replaceSync(text); } catch (e) { return Promise.reject(e); }
        return Promise.resolve(this);
    }
}
function ownedSheet(element) {
    const sheet = new CSSStyleSheet();
    sheet._owner = element;
    return sheet;
}

/* The computed style: what the browser's cascade gives the element (the
   properties it knows), then its inline style. */
function getComputedStyle(element, pseudo) {
    if (!(element instanceof Element))
        throw new TypeError('getComputedStyle: not an Element');
    flushSheets();
    const values = (!pseudo && N.computed(element)) || {};
    const inline = element.style;
    const get = name => {
        name = KEBAB(String(name));
        if (name === 'css-float') name = 'float';
        if (Object.prototype.hasOwnProperty.call(values, name))
            return values[name];
        return inline.getPropertyValue(name).replace(/\s*!important$/, '');
    };
    const names = Object.keys(values);
    const api = {
        getPropertyValue: name => get(name),
        getPropertyPriority: () => '',
        item: i => names[i] || '',
        setProperty() {
            throw new DOMException('Read-only', 'NoModificationAllowedError');
        },
        removeProperty() {
            throw new DOMException('Read-only', 'NoModificationAllowedError');
        },
        get length() { return names.length; },
        get cssText() { return ''; },
        get parentRule() { return null; },
    };
    return new Proxy(api, {
        get(target, key) {
            if (typeof key !== 'string') return target[key];
            if (key in target) return target[key];
            if (/^\d+$/.test(key)) return target.item(+key);
            return get(key === 'cssFloat' ? 'float' : key);
        },
        set() { return true; },
        has(target, key) { return typeof key === 'string'; },
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
    Storage, AbortController, AbortSignal, MutationObserver, DOMException,
    MutationRecord, customElements, Image, Option, Audio,
    fetch, Request, Response, Headers, XMLHttpRequest, crypto, Crypto,
    XMLHttpRequestEventTarget, FormData, Blob, File, TextEncoder,
    TextDecoder, DOMRect, DOMRectReadOnly, StyleSheet, CSSStyleSheet,
    CSSRule, CSSStyleRule, CSSGroupingRule, CSSMediaRule: CSSGroupingRule,
    CSSSupportsRule: CSSGroupingRule,
    ResizeObserver, IntersectionObserver, PerformanceObserver,
    MediaQueryList, Window,
    localStorage: storageProxy(persistent),
    sessionStorage: storageProxy(new Storage()),
    name: '', closed: false, isSecureContext: true, origin: '',
    devicePixelRatio: 1, length: 0,
    get innerWidth() { return N.viewportWidth(); },
    get innerHeight() { return N.viewportHeight(); },
    get outerWidth() { return N.viewportWidth(); },
    get outerHeight() { return N.viewportHeight(); },
    get scrollX() { return 0; },
    get scrollY() { return N.metrics()[1]; },
    get pageXOffset() { return 0; },
    get pageYOffset() { return N.metrics()[1]; },
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
    scroll(x, y) { G.scrollTo(x, y); },
    scrollTo(x, y) {
        const top = x && typeof x === 'object' ? x.top : y;
        if (top !== undefined) N.scrollTo(Number(top) || 0);
    },
    scrollBy(x, y) {
        const dy = x && typeof x === 'object' ? x.top : y;
        N.scrollTo(N.metrics()[1] + (Number(dy) || 0));
    },
    moveTo() {}, resizeTo() {},
    /* Called bare (addEventListener(...)), they act on the window. */
    addEventListener: (type, listener, options) =>
        EventTarget.prototype.addEventListener.call(G, type, listener, options),
    removeEventListener: (type, listener, options) =>
        EventTarget.prototype.removeEventListener.call(G, type, listener,
                                                       options),
    dispatchEvent: event => EventTarget.prototype.dispatchEvent.call(G, event),
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
    /* Saves localStorage before the page goes. */
    flush: flushStorage,
    /* True while requests wait for the next round. */
    waiting: () => netQueue.length > 0,
    /* A module's evaluation: its error (also after top-level await) is
       reported. */
    settle(promise) { Promise.resolve(promise).then(null, reportError); },
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
        if (type === 'resize' && resizeObservers.size > 0) checkResizes();
    },
};
})
