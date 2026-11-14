# OPOS browser plan

Status (2026-11-07): milestones 1, 2, 3 and 5 are done. Milestones 4 and 6 are mostly done; see the remaining work below.

The browser is a native SDK program, `/bin/browser`, built from `sdk/browser/`. It needs no new kernel code. It builds on what the SDK already has: sockets, DNS, TCP, HTTPS through BearSSL, the PNG/JPEG/BMP decoders in `opos_image`, the window API (key, mouse and wheel events), a heap, and the shared clipboard.

## Constraints

- The system font is a 5x9 ASCII bitmap at integer scales. The browser uses stb_truetype and the Noto Sans fonts in `/share/fonts/` instead.
- SDK programs are compiled with SSE enabled, so `float` and `double` work for layout. The SDK `printf` cannot print floating-point numbers.
- There is no JavaScript engine. Pages that need JavaScript will render poorly or not at all. This is expected, and the plan does not include JavaScript until much later.
- A program may map at most 1 GiB (`USER_MMAP_MAX_PAGES`; 64 MiB before OPOS 0.6.0), as far as free memory allows.
- The network stack is polled and socket calls move at most 64 KiB each, so large pages load slowly. RX interrupts would help and are a separate task.

## Milestones

Each milestone is tested in QEMU before the next one starts.

1. **HTTP client library.** Done. `sdk/lib/http.c` (`opos_http_get`: redirects, chunked transfer decoding, gzip through `sdk/lib/inflate.c`, a progress hook) and `opos_url_resolve`. `/bin/fetch` uses it.
2. **HTML parser.** Done. `sdk/lib/html.c`: a forgiving tokenizer and tree builder with implied end tags, entities and raw-text elements. Tested on the host with ASan/UBSan and a mutation fuzzer.
3. **Fonts, text and layout without CSS.** Done. Block and inline flow with word wrap, headings, lists, links, `<pre>`, images and tables.
4. **Browser shell.** Mostly done. Done: address bar (Ctrl+L selects it; Ctrl+C/V copy and paste), back, forward, reload (Ctrl+R), link clicking, hover URL in the status bar, text selection by dragging, Ctrl+A, Ctrl+C to the shared clipboard, find in page (Ctrl+F; Enter and Shift+Enter step through matches, Escape closes), error pages, including one for certificate problems. Not done: tabs, the page title in the window caption (needs a window-title call), `#fragment` scrolling.
5. **CSS subset.** Done. `css.c` and `style.c`: selectors, specificity, the cascade, custom properties, and colour, background, font, margin, padding, border, width, display, float, flex and a grid subset. Lengths: px, pt, em, rem, %, vw, vh, vmin and vmax.
6. **Forms and tables.** Mostly done. Tables and flexbox lay out. Forms work: click or Tab into a field and edit at a visible caret; arrows, Home, End, Delete, Backspace, mouse/Shift selection and Ctrl+A/C/X/V work in text fields and text areas. Password fields stay masked, checkboxes and radio buttons toggle, and selects open a drop-down list. A submit button or Enter in a text field sends the form by GET or POST (`application/x-www-form-urlencoded`, `formaction`/`formmethod` too). Tested with a local echo server and a DuckDuckGo Lite search. Not done: `multipart/form-data`, file uploads and reset buttons.
7. **Later.** Cookies, downloads, bookmarks, history, and a JavaScript engine (a separate project).

## Test pages

example.com, news.ycombinator.com, lobste.rs, www.gnu.org, lite.duckduckgo.com/lite/?q=opos and en.wikipedia.org/wiki/Operating_system all load and render readably. Known problem: Wikipedia shows its full table of contents before the article, because the page places it in a sidebar.

## Risks

- Most modern sites depend on JavaScript. The browser will handle documents, articles and simple pages well, and little beyond that.
- A CSS engine written from scratch is large. A limited subset and a fixed test list keep it manageable.
