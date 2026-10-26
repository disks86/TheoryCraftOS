# Web conformance tests

The HTML, CSS, JavaScript and WebAssembly implementations are checked against the same kinds of data that Chromium, Firefox and Ladybird
(LibWeb) run: web-platform-tests (WPT), test262, html5lib-tests and the WebAssembly spec tests. Nothing is downloaded; the suites are written
in the same style (and from the same spec sections) as the upstream directories they are named after.

Run them with `ONLY_WPT=1 PageTest` (the `PageTest` host test, built by `tests/run_host_tests.sh PageTest`) and
`tests/run_host_tests.sh LayoutConformanceTest`. For one file while iterating, `tests/data/wpt/run.sh tests/data/wpt/<file>.js` runs it against a
PageTest binary (`PAGETEST=path`); the console log of a page is capped at 16 KB, so a file that fails a lot may need running in parts.

| File | Modelled on | Covers |
| --- | --- | --- |
| `js_language.js` | test262 `language/` and `built-ins/` | destructuring, classes, generators, async, Proxy/Reflect, Symbol, BigInt, RegExp, Date, JSON, typed arrays, Array/String/Object builtins, Promise, iterator helpers, Intl, global object shape |
| `js_strict.js` | test262 strict mode, `built-ins/Object` integrity levels and `defineProperty` | strict mode semantics, property attributes, `preventExtensions`/`seal`/`freeze`, `Reflect` |
| `js_modules.js` | test262 `language/module-code`, WPT `the-script-element/module` | `import()`, namespace objects, live bindings, `export … from`, top-level await, `import.meta`, `<script type=module>`, `nomodule`, import maps |
| `wasm_jsapi.js` | WPT `wasm/jsapi` | the `WebAssembly` namespace, Module/Instance/Memory/Table/Global/Tag/Exception, error classes, compile/instantiate, imports and linking, conversions, traps |
| `html_parser.js` | html5lib-tests tree construction, WPT `html/syntax` | tokenizer states, character references, insertion modes, adoption agency, foster parenting, foreign content, templates, quirks mode, serialisation |
| `css_conformance.js` | WPT `css/` | cascade, specificity, layers, nesting, custom properties, values and units, `calc()`, colours, display, backgrounds, text, selectors, `@media`/`@supports`/`@container`, the UA style sheet, CSSOM restyling, `matchMedia`, `CSS.*` |
| `tests/LayoutConformanceTest.cpp` | WPT `css/CSS2`, `css-flexbox`, `css-grid`, `css-tables` | box geometry: margins and their collapsing, floats, positioning, inline-blocks, flex, grid, tables (geometry is only available after a page is rendered, so it is tested on the box tree) |
| `dom_ranges_traversal.js` | WPT `dom/ranges`, `dom/traversal`, `selection`, `MutationObserver` | Range, StaticRange, Selection, TreeWalker, NodeIterator, MutationObserver, shadow roots |
| `web_apis.js` | WPT `idlharness` | the interface objects, prototype chains and members of ~350 platform interfaces |
| `web_apis_behavior.js` | WPT `geometry`, `encoding`, `WebCryptoAPI`, `hr-time`, `user-timing`, `FileAPI`, `IndexedDB`, `workers`, `eventsource`, `html/semantics` | DOMMatrix/DOMRect, TextEncoder/Decoder, atob/btoa, SubtleCrypto, performance, EventTarget, AbortSignal, Blob/File, Web Storage, Worker, IndexedDB, EventSource, popover, dialog, custom elements, canvas 2D |
| `dom_nodes.js` … `messaging_streams.js` | WPT `dom`, `url`, `fetch`, `html`, `streams`, … | the earlier suites (see `tests/data/wpt/`) |

## Known gaps

A test for something the engines do not do yet is listed with `xfail("name")` before the test. It must still fail; when it starts to pass the
run reports `XPASS name` and fails, so the entry gets deleted and the gap is closed for good. The registry is therefore the list of conformance
gaps: `grep -c xfail tests/data/wpt/*.js`. The largest ones today:

- **JavaScript**: ES modules work (`import`/`export`, `import()`, `import.meta`, top-level await, `<script type=module>`, `nomodule`, import maps; the
  loader is `module_prelude()` in `WebPrelude.cpp` over `__compile_module`). Strict mode (`"use strict"`, modules, class bodies), non-extensible and sealed
  objects and non-configurable properties are implemented (`js_strict.js`); the host's own scripts keep configurable-by-default properties
  (`FunctionCode::library`). Left: the `prototype` of built-in constructors is configurable, module namespace properties are accessors
  (not writable data properties), no strict-only early errors (octal literals, duplicate parameters, `eval`/`arguments` assignment). Also missing:
  `BigInt64Array`, `SharedArrayBuffer`/`Atomics`, resizable/transferable `ArrayBuffer`, iterator helpers, `Intl` beyond basic formatting, RegExp
  lookbehind / property escapes / `v` flag / `d` flag / duplicate named groups, `Object.prototype.toString` tags for Date, Map, Set and arguments,
  TDZ, per-iteration `let` bindings in `for`, sloppy `arguments` mapping, `Symbol.match/split/unscopables`, `String.prototype.normalize`/special
  casing, lone-surrogate handling, `new.target`/`Reflect.construct`, tagged template caching, `for…in` integer key order, and a call depth of 500.
- **HTML**: the parser follows the standard (`src/Dom/HtmlBuilder.cpp`: tokenizer states, insertion modes, adoption agency, foster parenting, foreign
  content, templates, quirks mode). Left: the doctype is not a node (`document.doctype`, `publicId`/`systemId`), `DOMParser` with XML, character
  references that stand for two code points, `innerHTML` scripts must not run.
- **CSS**: computed values are often not resolved (`flow-root`, blockified `display`, `line-height`, `text-align`, `transform-origin`), UA style sheet
  values (heading sizes, `ul` padding, link colour, `em`), newer colour spaces (`lab`, `oklch`, `color-mix`, relative colours), `lh`, nesting,
  `revert-layer`, `@container` failure, `:dir()`, `:nth-child(… of S)`, `aspect-ratio`, `flow-root` floats, many properties return `""`
  (`text-indent`, `word-break`, `quotes`, `counter-*`, `contain`, `will-change`, …).
- **Web APIs**: ~100 interfaces are missing (Web Audio, Web Animations, FontFace, most SVG DOM classes, `StaticRange`, IndexedDB cursors/ranges, …) and for
  most existing interfaces the constructor `[[Prototype]]` chain does not follow the interface hierarchy.
- **WebAssembly JS API**: `WebAssembly` error classes do not inherit from `Error` (the constructor), constructors' `prototype` is writable, Table and
  Global argument edge cases, structured clone of `Module`.

## Writing a test

`tests/data/wpt/harness.js` is a small testharness.js (`test`, `async_test`, `promise_test`, the `assert_*` functions). Put a new file in
`tests/data/wpt/`, add its name to `test_wpt()` in `tests/PageTest.cpp`, and keep each test about one behaviour: a failure inside a test hides the
rest of that test, and an `xfail` covers the whole test. Dense table-driven files (`css_conformance.js`, `html_parser.js`, `web_apis.js`) register
one test per row for that reason.

Tests that need no DOM can be checked against a reference engine before they are trusted: concatenate `harness.js` and the file and run it with
`node` (the JavaScript, module and WebAssembly files were verified that way; for modules use `data:` URLs in place of `mod_url`). Every `xfail`
should then report `XPASS` in node.
