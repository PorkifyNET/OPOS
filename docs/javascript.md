# JavaScript in the browser

Most of the web needs JavaScript: menus, buttons, search boxes, sign-in
pages and whole sites built with React, Vue or Next.js. The OPOS browser
runs page scripts in **QuickJS**, a small, complete JavaScript engine
(ES2023, MIT licence) copied unchanged into `sdk/third_party/quickjs`.
OPOS writes the part that makes a browser out of it: the DOM, events,
timers and the network APIs.

## Design

- **One engine per page.** Each page gets its own QuickJS runtime and
  context when it is shown (`script_page_start`), freed with the page.
  Pages cannot see each other, and a page that runs out of its 384 MiB
  JavaScript limit only stops its own scripts.
- **The DOM is written in JavaScript.** `sdk/browser/dom.js` (built into
  the browser) defines `Node`, `Element`, `HTMLElement`, `Text`, `Event`,
  `document`, `window`, `location`, `URL` and the rest on top of about 40
  native calls in `sdk/browser/script.c` ("N.parent", "N.attr",
  "N.insert", "N.query" ...). Adding an API usually means only changing
  `dom.js`.
- **Scripts change the real document tree.** Script calls edit the
  `opos_html_node` tree from `opos_html.h` (new editing calls in
  `sdk/lib/html.c`). After every script, timer or event that changed it,
  the browser drops its cached CSS matches and lays the page out again.
  Each node has one wrapper object for the page's life, so `a === b` holds
  for the same element and properties set on it stay.
- **Nothing waits.** Scripts run one at a time on the browser's own
  thread. A script, timer or event gets 5 seconds before it is stopped
  (an "interrupted" error); time spent waiting for the network does not
  count. Navigation that a script asks for (`location.href = ...`)
  happens once it returns. `Atomics.wait` is off.
- **Memory.** QuickJS's many small blocks come from per-size free lists
  (`script.c`), because the SDK `malloc` keeps one sorted list and slows
  down with hundreds of thousands of blocks.
- **SDK C library.** QuickJS is built with only SDK headers. This added to
  the SDK: `FILE` output (`stdout`, `stderr`, `fprintf`), `%f` in printf,
  `<time.h>`/`<sys/time.h>` (UTC only), `<errno.h>`, `<inttypes.h>`,
  `<setjmp.h>`, `<fenv.h>`, `<malloc.h>`, `abort`, `alloca`, and the rest
  of `<math.h>` (tan, atan2, asin, acos, sinh..., cbrt, hypot, log2,
  expm1, log1p, round, trunc, lrint, fmin, fmax and others).
- **Off switch.** The "=" menu has "Turn JavaScript off/on" (kept as the
  file `javascript-off` in `~/AppData/Browser`). With JavaScript on,
  `<noscript>` is hidden and `@media (scripting: enabled)` matches.

## Steps

1. **Engine, DOM and events** (done, branch `browser/javascript`)
   - Classic scripts, inline and `src`, run in document order once the
     page is parsed (`async` and `defer` make no difference yet); then
     `DOMContentLoaded` and `load`. Scripts that a script inserts run
     after it. `type="module"` scripts do not run yet.
   - DOM: tree walking, `getElementById`, `querySelector(All)`,
     `matches`, `closest`, `getElementsBy...`, creating, inserting,
     moving, cloning and removing nodes, `textContent`, `innerHTML`,
     `outerHTML`, `insertAdjacentHTML`, attributes, `classList`,
     `dataset`, `style` (as the `style` attribute), form values,
     `checked` and `selectedIndex`, `document.title`, `document.write`.
   - Events: `addEventListener` with capture, bubbling, `once`,
     `preventDefault` and `stopPropagation`; `onclick="..."` attributes
     and `el.onclick = f`. The browser sends `click` (links, buttons,
     text, check boxes), `input` and `change` from form fields,
     `keydown`/`keypress`/`keyup`, and `submit` before a form is sent. A
     cancelled click does not follow the link; `javascript:` links run.
   - Window: `setTimeout`/`setInterval`, `requestAnimationFrame`,
     promises and `queueMicrotask`, `console` (to the Terminal),
     `location`, `URL`, `URLSearchParams`, `navigator`, `screen`,
     `localStorage`/`sessionStorage` (step 2 keeps `localStorage`),
     `matchMedia`, `atob`/`btoa`, and stand-ins for
     `IntersectionObserver` (reports everything visible),
     `MutationObserver` and `ResizeObserver` (never report).
   - Tested in QEMU: a test page (tree changes, external script, timer,
     clicks, a cancelled link, typing, 19 language checks) works fully;
     GitHub, Hacker News and BBC News load and run their scripts without
     crashing.
2. **Network and storage** (done, branch `browser/javascript-net`)
   - `fetch` (`Request`, `Response`, `Headers`, `AbortController`) and
     `XMLHttpRequest` (async and sync, `responseType` text, json,
     arraybuffer and blob). Requests wait in a queue and run one by one
     in the main loop between timers and events; the page waits while one
     runs (there are no threads). Bodies: strings, `URLSearchParams`,
     `FormData` (sent as multipart), `Blob`, `ArrayBuffer` and typed
     arrays. Also `TextEncoder`, `TextDecoder`, `Blob`, `File`,
     `FormData` and `DOMException`.
   - The SDK HTTP library gained `opos_http_request` (any method, extra
     headers, the response's headers in `response.headers`).
   - Safety: cookies go only with requests to the page's own site
     (scheme, host and port); requests to other sites never carry
     cookies, because SameSite is not tracked. Requests to other sites
     send `Origin`; requests that are not "simple" (other methods or
     headers) are first asked with `OPTIONS` (CORS preflight); answers
     reach the page only with a matching `Access-Control-Allow-Origin`.
     Pages cannot set `Cookie`, `Host`, `Origin`, `Referer` and the other
     forbidden headers, and never see `Set-Cookie`.
   - `document.cookie` reads and sets cookies without the HttpOnly ones;
     the jar records HttpOnly (a ninth field in `cookies.txt`; cookies
     saved before this stay hidden from scripts).
   - `localStorage` is kept per site in `~/AppData/Browser/storage/`
     (about 5 MB each), saved after each timer round that changed it and
     when the page closes. Tabs of the same site do not see each other's
     changes until they load again. `sessionStorage` lasts for the page.
     Both also work as `localStorage.key = value`.
   - `history.pushState`/`replaceState` change the address bar and the
     tab's back list without loading; Back to such an entry loads it
     (there is no `popstate` yet). `history.back()`, `forward()` and
     `go(n)` work like the toolbar buttons.
   - Tested in QEMU with a test server on two ports (two origins): 22
     checks pass (fetch, redirects, XHR, POST bodies, HttpOnly, CORS
     allowed/blocked/preflight, abort, storage kept after reload,
     pushState). BBC News still loads.
3. **Styles and layout from scripts** (done, branch `browser/javascript-css`)
   - Style sheets follow the document: before each layout the browser
     compares the page's `<style>` and `<link rel="stylesheet">`
     elements with the sheets it has (`sheets_update` in `main.c`),
     parses or fetches new ones, keeps unchanged ones and relinks the
     chain (`css_relink`). Scripts can add, change, remove and disable
     sheets; a script-added link fires `load` once it is in.
   - CSS object model: `style.sheet` and `link.sheet`
     (`CSSStyleSheet`), `cssRules`, `insertRule`/`deleteRule`,
     `addRule`, rules' `selectorText` and `style`, `@media` and
     `@supports` groups with their own rules, `document.styleSheets`,
     `new CSSStyleSheet()` with `replaceSync` and
     `document.adoptedStyleSheets`. Rules changed this way reach the
     browser as text (`N.setSheet`) until the element's own text changes.
     This is what CSS-in-JS libraries (styled-components, emotion) use.
     Linked sheets from other sites throw `SecurityError` on `cssRules`;
     same-site linked sheets show no rules yet.
   - Layout from scripts: every block, table and cell leaves an
     invisible box item (`ITEM_BOX`) in the display list, so
     `getBoundingClientRect`, `getClientRects`, `offsetWidth/Height/
     Top/Left`, `offsetParent` (the body, or null when not shown),
     `clientWidth/Height` and `scrollWidth/Height` give real sizes;
     inline elements get the bounds of their text. Asking for a size
     after a change lays the page out first, as browsers do.
   - `getComputedStyle` runs the real cascade for the element (display,
     position, colours, font, margins, padding, borders, width and
     height from the layout, flex, transform and more; other properties
     come from the inline style).
   - Scrolling: `scrollX/scrollY`, `scrollTo`, `scrollBy`,
     `scrollIntoView` and the root's `scrollTop`; the window gets
     `scroll` when the user scrolls and `resize` when it changes size.
   - Also: `DOMRect`, `crypto.getRandomValues` and `randomUUID` (from
     the system's `getrandom`), a settable `defaultChecked`, and bare
     `addEventListener(...)` calls acting on the window.
   - The browser now runs on a 4 MiB stack (`opos_stack_size`, read by
     the SDK's `crt0.S`; other programs keep 128 KiB) and gives
     JavaScript 1 MiB of it. React's hydration ran past the old 128 KiB
     and crashed the browser.
   - Not yet: positions relative to the nearest positioned box
     (`offsetTop` is from the page), `visibility: hidden` boxes, CSS
     custom properties in `getComputedStyle`, element scrolling
     (`overflow: auto`), `crypto.subtle`.
   - Tested in QEMU: a test page with 17 checks (sizes, display none,
     computed styles, insertRule/deleteRule, rule.style, added and
     removed `<style>`, `disabled`, adopted sheets, scrolling, a
     script-added link's `load`, crypto) passes; the step 2 network test
     still passes 22/22; BBC News now hydrates and shows its full front
     page (before, React gave up with "Application error"); GitHub and
     Hacker News load without script errors.
4. **Modules and more** (done): `type="module"` and `import`,
   `async`/`defer` order, `MutationObserver` that reports, `<template>`
   and custom elements.
   - Modules: `<script type="module">` (inline or `src`), static and
     dynamic `import`, `import.meta.url`, top-level `await`. QuickJS
     loads each module once per page through `JS_SetModuleLoaderFunc`;
     the browser fetches it with the page's cookies. `<script
     type="importmap">` maps bare names (exact keys and `"prefix/"`
     keys); an unmapped bare name throws `TypeError`. Errors in a
     module (also after `await`) are reported through the `settle` hook.
     `nomodule` scripts are skipped. `HTMLScriptElement.supports`.
   - Order: scripts run in two passes as in browsers. First the
     classic scripts and async scripts in page order, then `defer`
     scripts with `src` and non-async modules in page order.
   - Every change to the tree goes through `N.insert`, `N.remove`,
     `N.setAttr`, `N.removeAttr` and `N.setData`. dom.js wraps them;
     while observers or custom elements exist, the wrappers record the
     change. With neither, nothing extra runs.
   - `MutationObserver`: `childList`, `attributes` (with
     `attributeFilter` and `attributeOldValue`), `characterData`,
     `subtree`; records come in a microtask; `takeRecords`,
     `disconnect`.
   - `ResizeObserver`: sizes from the layout, checked after observe,
     after each timer round and on window `resize`.
   - `<template>`: `content` is a fragment (the children move there the
     first time); `innerHTML` on a template reads and writes the
     content; scripts inside templates never run.
   - Custom elements: `customElements.define/get/getName/whenDefined/
     upgrade`, `class X extends HTMLElement` with `new X()`,
     `createElement` and upgrades of elements already on the page;
     `connectedCallback`, `disconnectedCallback`,
     `attributeChangedCallback` for `observedAttributes`.
   - Selectors: CSS escapes (`.md\:flex`, `.w-1\/2`, `.\31 0`, as
     Tailwind uses) and `:not(a, b)` with compound selectors (it
     counts as its most specific argument). Selector errors now name
     the selector.
   - Also: `new Image()`, `new Option()`, `new Audio()`.
   - Not yet: shadow DOM (`attachShadow`), customized built-ins
     (`is="..."`), `:not()` with combinators inside, module scripts in
     workers, `import` attributes (JSON modules).
   - Tested in QEMU: a test page with 15 checks (import map, static and
     dynamic import, import.meta, nomodule, bad import, run order,
     MutationObserver, takeRecords, template, custom elements,
     ResizeObserver, escaped selectors, `:not()`, `new Image`) passes;
     the step 3 CSS test still passes 17/17 and the network test 22/22;
     BBC News and GitHub load without script errors (GitHub's popover
     code needed `:not([attr])` and escaped selectors).
5. **Canvas 2D** (done, branch `browser/javascript-canvas`)
   - `<canvas>` is now shown (it was `display: none`) as a replaced
     element like `<img>`, 300 x 150 unless its `width`/`height` say
     otherwise. `sdk/browser/canvas.c` keeps one 0xAARRGGBB surface per
     canvas; layout reads it through `canvas_pixels` and the browser
     paints again whenever a canvas changed (`canvas_take_painted`).
   - The rasteriser is the one `svg.c` already had, moved into
     `vector.h` and shared: paths flattened to device pixels, exact-area
     anti-aliasing, strokes with caps, joins and miter limits.
   - `CanvasRenderingContext2D` (in `dom.js`, one native `N.cv`):
     rectangles, paths (`arc`, `arcTo`, `ellipse`, `roundRect`, Bézier
     curves), `fill`/`stroke` with nonzero and even-odd rules, `clip`,
     `save`/`restore`, transforms (`DOMMatrix`, `getTransform`,
     `setTransform`), `globalAlpha`, composite operations and blend
     modes, line dashes, linear, radial and conic gradients, patterns,
     `fillText`/`strokeText`/`measureText` with the browser's fonts,
     `drawImage` from `<img>`, `<canvas>` and `ImageBitmap`,
     `getImageData`/`putImageData`/`createImageData`,
     `isPointInPath`/`isPointInStroke`, `Path2D` (also from SVG path
     data), `toDataURL` and `toBlob` (PNG), and `createImageBitmap`.
     Setting `width` or `height` clears the canvas and its state.
   - Images: `new Image()` and `<img>` load for scripts with `onload`/
     `onerror`, `complete` and `naturalWidth`. Images from other sites
     taint the canvas: `getImageData` and `toDataURL` then throw
     `SecurityError`.
   - `Intl` (QuickJS has none): English formatting for `NumberFormat`
     (decimal, currency, percent, compact), `DateTimeFormat`,
     `Collator`, `PluralRules`, `RelativeTimeFormat`, `ListFormat`,
     `Segmenter` and `Locale`, plus `toLocaleString`. Chart libraries
     need it.
   - Not yet: shadows and `filter` (kept, not drawn), WebGL,
     `OffscreenCanvas`, other locales.
   - Tested in QEMU: a test page with 24 checks (fills, strokes, arcs,
     transforms, clip, alpha, compositing, gradients, text, drawImage,
     toDataURL round trip, putImageData, Path2D, dashes, patterns, width
     reset, same-site and cross-site images, broken images, Intl)
     passes; Chart.js 4.4 from a CDN draws a bar chart and a doughnut;
     the network (22), CSS (17) and module (15) tests still pass.
6. **Speed**: keep compiled bytecode of large scripts for the session,
   lay out only once per batch of changes, and measure heavy sites.

## Not planned

Plug-ins, WebAssembly, WebGL and service workers stay out until the
steps above are done.
