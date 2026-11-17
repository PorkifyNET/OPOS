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
3. **Styles and layout from scripts**: `<style>` and `<link>` sheets
   added by scripts, `HTMLStyleElement.sheet` with `insertRule` (CSS in
   JS, such as styled-components), `getComputedStyle` from the real
   cascade, and element sizes and positions from the layout
   (`getBoundingClientRect`, `offsetWidth`, `scrollTop`), so menus and
   pop-ups can place themselves.
4. **Modules and more**: `type="module"` and `import`, `async`/`defer`
   order, `MutationObserver` that reports, `<template>` and custom
   elements, `<canvas>` 2D drawing.
5. **Speed**: keep compiled bytecode of large scripts for the session,
   lay out only once per batch of changes, and measure heavy sites.

## Not planned

Plug-ins, WebAssembly, WebGL and service workers stay out until the
steps above are done.
