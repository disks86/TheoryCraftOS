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
| `js_builtins.js` | test262 `built-ins/TypedArrayConstructors`, `built-ins/Iterator`, `built-ins/RegExp` | BigInt64Array/BigUint64Array and DataView BigInt accessors, array/string iterators and the iterator helpers, RegExp lookbehind and `\p{…}` |
| `js_modules.js` | test262 `language/module-code`, WPT `the-script-element/module` | `import()`, namespace objects, live bindings, `export … from`, top-level await, `import.meta`, `<script type=module>`, `nomodule`, import maps |
| `wasm_jsapi.js` | WPT `wasm/jsapi` | the `WebAssembly` namespace, Module/Instance/Memory/Table/Global/Tag/Exception, error classes, compile/instantiate, imports and linking, conversions, traps |
| `html_parser.js` | html5lib-tests tree construction, WPT `html/syntax` | tokenizer states, character references, insertion modes, adoption agency, foster parenting, foreign content, templates, quirks mode, serialisation |
| `css_conformance.js` | WPT `css/` | cascade, specificity, layers, nesting, custom properties, values and units, `calc()`, colours, display, backgrounds, text, selectors, `@media`/`@supports`/`@container`, the UA style sheet, CSSOM restyling, `matchMedia`, `CSS.*` |
| `tests/LayoutConformanceTest.cpp` | WPT `css/CSS2`, `css-flexbox`, `css-grid`, `css-tables` | box geometry: margins and their collapsing, floats, positioning, inline-blocks, flex, grid, tables (geometry is only available after a page is rendered, so it is tested on the box tree) |
| `dom_ranges_traversal.js` | WPT `dom/ranges`, `dom/traversal`, `selection`, `MutationObserver` | Range, StaticRange, Selection, TreeWalker, NodeIterator, MutationObserver, shadow roots |
| `web_apis.js` | WPT `idlharness` | the interface objects, prototype chains and members of ~350 platform interfaces |
| `web_animations.js` | WPT `web-animations/` | the Animation state machine, timing, keyframes, easing, and the values that reach the style |
| `css_font_loading.js` | WPT `css/css-font-loading` | `FontFace`, `document.fonts`, loading status, events, `check()` |
| `css_animations.js` | WPT `css/css-animations`, `css/css-transitions` | `CSSAnimation` / `CSSTransition` from style, their events, cancelling, ordering |
| `web_audio.js` | WPT `webaudio/` | offline rendering of the graph: parameters and their automation, oscillators, sources, filters, delay, convolver, analyser, worklet, decoding |
| `dom_svg.js` | WPT `svg/types`, `svg/geometry` | the SVG DOM interfaces, animated attributes, lengths, transform lists, geometry and text methods |
| `web_apis_behavior.js` | WPT `geometry`, `encoding`, `WebCryptoAPI`, `hr-time`, `user-timing`, `FileAPI`, `IndexedDB`, `workers`, `eventsource`, `html/semantics` | DOMMatrix/DOMRect, TextEncoder/Decoder, atob/btoa, SubtleCrypto, performance, EventTarget, AbortSignal, Blob/File, Web Storage, Worker, IndexedDB, EventSource, popover, dialog, custom elements, canvas 2D |
| `dom_nodes.js` … `messaging_streams.js` | WPT `dom`, `url`, `fetch`, `html`, `streams`, … | the earlier suites (see `tests/data/wpt/`) |

## Known gaps

A test for something the engines do not do yet is listed with `xfail("name")` before the test. It must still fail; when it starts to pass the
run reports `XPASS name` and fails, so the entry gets deleted and the gap is closed for good. The registry is therefore the list of conformance
gaps: `grep -c xfail tests/data/wpt/*.js`. The largest ones today:

- **JavaScript**: ES modules work (`import`/`export`, `import()`, `import.meta`, top-level await, `<script type=module>`, `nomodule`, import maps; the
  loader is `module_prelude()` in `WebPrelude.cpp` over `__compile_module`). Strict mode (`"use strict"`, modules, class bodies), non-extensible and sealed
  objects and non-configurable properties are implemented (`js_strict.js`); the host's own scripts keep configurable-by-default properties
  (`FunctionCode::library`). The `prototype` of functions, classes and built-in constructors is non-configurable (read-only for classes and built-ins). Left: module namespace properties are accessors
  (not writable data properties). Strict-only early errors are reported by the compiler (`strict_name` and friends in `Compiler.cpp`): legacy octal literals and
  escapes (which are octal in sloppy code now), duplicate parameters, `eval`/`arguments` as a binding or assignment target, `delete name`, the words reserved in
  strict code, a `"use strict"` directive with non-simple parameters, a function declaration as the body of an `if` or a label. A name that a pattern parameter repeats is a duplicate too. Not done: `let` as an identifier
  in sloppy code. A strict `eval` has a scope of its own (`compile_program(..., local_scope)`). Also missing:
  `SharedArrayBuffer`/`Atomics` are single-threaded stand-ins (no `wait`/`notify` blocking, `instanceof ArrayBuffer` is true); missing: `Intl` date and number formatting beyond English and a few numeric orders (the Collator, `Locale`, `Segmenter`, `DisplayNames` and `getCanonicalLocales` are there, with English names and an outline of the UAX 29 and collation rules), the RegExp `v` flag (set notation), a non-`u` match that starts or ends between the two halves of a surrogate pair (the matcher is right about it - `/^.$/` fails on an emoji, `/^..$/` matches - but the match is reported as the whole character, so `/./g` finds one match in an emoji where a browser finds two), `\p` beyond the general categories, the main scripts and some binary properties, top-level `let`/`const` temporal dead zone (they are globals), direct `eval` that sees local variables, `Symbol` wrapper in object spread, and `for…in` on a proxy calling `getPrototypeOf`. Closed since the first pass: mapped `arguments`, `with`, redeclaration errors, wrapper objects, `Reflect.construct`/`new.target`, `super` in object literals, `#x in obj`, destructuring closing its iterator, holes, symbols, proxies with all traps and invariants.
- **HTML**: the parser follows the standard (`src/Dom/HtmlBuilder.cpp`: tokenizer states, insertion modes, adoption agency, foster parenting, foreign
  content, templates, quirks mode). The doctype is kept (`document.doctype` with `publicId`/`systemId`; `compatMode` is `BackCompat` in quirks mode; limited-quirks is not told
  apart), all 2125 named character references are known (`src/Dom/EntityTable.inc`, generated from the standard's table, including the two code point ones), a
  malformed XML document from `DOMParser` has a `parsererror` document element, `document.defaultView` is `null` outside the page's own document, and
  `outerHTML` on the document element throws `NoModificationAllowedError`. `html_parser.js` has no xfail left.
- **Platform interfaces** (`web_apis.js`, no xfail left): every interface of the idlharness table exists with its parent and members. Real behaviour was added
  for `CharacterData` data editing, `NamedNodeMap` (`setNamedItem`, `removeNamedItem`, `*NS`), `DocumentType` (a `Node`), `HTMLDocument` (the prototype of the page's
  document), `Document.parseHTMLUnsafe`, `ElementInternals`/`ValidityState` via `attachInternals()`, `URLPattern` (names, `*`, `(regexp)`, `{groups}` and `? + *`
  modifiers, base URLs; not the full constructor-string grammar), SVG `getPathData()` and polygon/polyline `points`, `Highlight`/`CSS.highlights` (registered, not painted),
  `form.elements`/`select.options` as `HTMLFormControlsCollection`/`HTMLOptionsCollection` (`add`, `remove`, `selectedIndex`; a form not in the document lists no controls), `dialog.requestClose()`, `getModifierState`, `PerformanceObserver.supportedEntryTypes`. Interface-only (calling the members is an illegal invocation, or there is
  nothing behind them): `MediaRecorder` (the constructor throws `NotSupportedError`: no encoder), `Worklet`, the worker global scope interfaces, `FileSystem*Handle`, `PublicKeyCredential`,
  `OffscreenCanvas.transferToImageBitmap`; `createScriptProcessor` returns a gain node whose `onaudioprocess` is never called. Interface objects are now not
  enumerable and have a read-only `prototype`.
- **CSS**: closed: `getComputedStyle` reports table parts (with `cellpadding`), controls, closed `dialog`, blockified `display`, `em`/`i`, `pre`, `ol`, link colour,
  `sub`/`sup`, `mark`; the properties that used to read `""` (`text-indent`, `white-space` longhands, `word-break`, `overflow-wrap`, `text-overflow`, `quotes`,
  `tab-size`, `list-style-position`, counters, `content`, `grid-*-start/end`, `column-count`, `mix-blend-mode`, `contain`, `accent-color`, `appearance`, `will-change`,
  `border-collapse`); `line-height: normal` and numbers that follow the font size; `text-align: start`; `font-style: oblique <angle>`; `aspect-ratio` as written;
  `transform-origin` and `background-position` keywords; `url()` and gradient colours serialized; `lh`; `all`; `revert` and `revert-layer`; `flow-root`; CSS nesting; logical properties with
  `direction` / `writing-mode`; `:nth-child(… of S)`, `:dir()`, `:read-write` on `contenteditable`; `calc()` needs spaces around `+` and `-`; an empty custom property;
  a `var()` cycle uses its fallback; `rgb()` rounds; `color` keeps `lab()`, `lch()`, `oklab()`, `oklch()`, `color()` and `color-mix()` in their own space and `rgb()`/`hsl()`/`hwb()`/`lab()`… relative colours (`from`) work for plain channel keywords (a `calc()` of them does not).
  Also closed: `getComputedStyle` is live (it reads the element each time) and read-only (`NoModificationAllowedError`, `cssText`), and takes `::before` / `::after` (`attr()` and strings in `content` are resolved,
  `counter()` stays as written); `HTMLStyleElement.disabled` is the sheet's flag; `@container` is evaluated against the nearest container ancestor whose `width` / `height` is a length (the viewport otherwise; no container
  ancestor: no match); `CSS.escape` per CSSOM, `CSS.px()` and the other numeric factories (`CSSUnitValue`), `CSS.registerProperty` (validated and recorded; its syntax and initial value are not applied to computed values yet).
  Still open: heading sizes, `ul` padding (40px), `blockquote` margin and `small` size (the UA metrics are terminal cells on purpose), `hr` border, other properties than `color` (`background-color`, borders) still read newer colour spaces as sRGB, unitless non-zero lengths (accepted as pixels, as in quirks mode)
- **Web APIs**: the constructor of an interface inherits from the constructor of its parent, `AbstractRange`/`StaticRange`, `EventSource`,
  `IDBVersionChangeEvent`, the device events, `DOMImplementation`, `NodeFilter` as an interface object, the legacy element interfaces and the
  constants of `WheelEvent`/`KeyboardEvent`/`DOMException` exist (`finish_prelude()` in `WebPrelude.cpp`, run last). The attributes the engine keeps on the
  objects are mirrored as placeholder accessors on the prototypes, so `'nodeType' in Node.prototype` is true (a read that reaches one gives undefined, a
  write makes the object's own property; they are not enumerable). The SVG DOM is there (`svg_prelude()` in `WebPrelude.cpp`: `SVGElement` and the interface of each element, animated lengths / numbers / strings / enumerations / `viewBox` / `transform`, `getBBox`, `getCTM`, `getTotalLength`, `getPointAtLength`, `isPointInFill`, text methods; text metrics use the 8 x 16 cell, there is no animation and `getScreenCTM` is `getCTM`). Web Audio is there too (`web_audio_prelude()` in `WebPrelude.cpp`: `AudioContext` / `OfflineAudioContext` with a graph rendered 128 frames at a time, `AudioBuffer`, `AudioParam` automation, the oscillator, buffer source, constant source, gain, delay, biquad, IIR, stereo panner, panner, wave shaper, convolver, compressor, analyser, splitter / merger, script processor and audio worklet nodes, `decodeAudioData` through the engine's decoders; the sound of an online `AudioContext` is rendered and handed to `__audio_sink(samples, channels, rate)` when the page environment defines one, and dropped otherwise, there is no output device yet; `HRTF` panning is equal power; the compressor is not bit exact; media element and stream sources are silent). The Web Animations API is there (`animation_prelude()` in `WebPrelude.cpp`: `Animation`, `KeyframeEffect`, `DocumentTimeline`, `element.animate()`, `getAnimations()`, the timing model, keyframe processing, easing, interpolation of numbers / lengths / colours / transforms / lists, composite add and accumulate, `commitStyles()`, `persist()` and automatic removal of replaced animations; the animated values are a layer on top of the cascade (`__set_animated_style`, applied by `derive_style`, over the style attribute and under `!important` rules)). CSS animations and transitions run on top of it: after a style change the prelude looks at every element, starts / updates / cancels `CSSAnimation` (from `@keyframes`, including ones inside `@media` / `@supports`) and `CSSTransition` objects (from changed computed values, `transition-behavior: allow-discrete` for non-interpolable ones), orders them transitions < CSS animations < script animations, and fires `animationstart` / `animationiteration` / `animationend` / `animationcancel` and `transitionrun` / `transitionstart` / `transitionend` / `transitioncancel`. The check runs when the document's style version moved (`__style_version`, which ignores the animated layer) and a style sheet or style attribute uses motion (`__motion_in_use`), before `getComputedStyle` / `getAnimations` and on the next frame; there are no pseudo-element animations, and no scroll or view timelines. The CSS Font Loading API is there too (`FontFace`, `FontFaceSet` as `document.fonts`, `FontFaceSetLoadEvent`: descriptor validation, loading from a buffer or the first fetchable `url()`, status and events, `load()` / `check()` with `unicode-range`; `@font-face` rules of the style sheets are members of `document.fonts`; the data is only checked for a font signature, the renderer does not use a `FontFace` made in script). Still missing:
  `DocumentType`/`document.doctype`, form/options collections as interfaces, most methods listed in `missing_members`, and the prototype-level
  accessors for attributes of `Window`.
- **WebAssembly JS API**: `Module` structured-clones (the bytes are kept); the other objects throw `DataCloneError`. Built-in methods are not
  constructors (a native is constructible only if it has a `prototype` property, or is `Proxy`), a function's `name` and `length` are read-only, and
  `ToNumber` of a Symbol throws (symbols are strings cells flagged `JsString::symbol`, see below).

## Writing a test

`tests/data/wpt/harness.js` is a small testharness.js (`test`, `async_test`, `promise_test`, the `assert_*` functions). Put a new file in
`tests/data/wpt/`, add its name to `test_wpt()` in `tests/PageTest.cpp`, and keep each test about one behaviour: a failure inside a test hides the
rest of that test, and an `xfail` covers the whole test. Dense table-driven files (`css_conformance.js`, `html_parser.js`, `web_apis.js`) register
one test per row for that reason.

Tests that need no DOM can be checked against a reference engine before they are trusted: concatenate `harness.js` and the file and run it with
`node` (the JavaScript, module and WebAssembly files were verified that way; for modules use `data:` URLs in place of `mod_url`). Every `xfail`
should then report `XPASS` in node.

## Symbols

A Symbol is a string cell with `JsString::symbol` set; its text is `@@<description>#<n>` (a well-known one is the interned `@@iterator` and the like, so the C++
side keeps looking them up by that text). `typeof` says "symbol", `to_string` / `to_number` throw, `String(sym)` and `Symbol.prototype.toString` give
`Symbol(description)`, a symbol is its own property key (never interned), and the string key lists (`Object.keys`, `for-in`, `getOwnPropertyNames`, JSON)
skip symbol keys while `getOwnPropertySymbols` / `Reflect.ownKeys` list them. `Symbol.for` is a Map in `Stdlib.cpp`. A string whose text equals a well-known
symbol's (`'@@iterator'`) is the same cell as the symbol. Not done: wrapper objects (`Object(sym)`), symbol keys in object spread / `Object.assign`.

## Strings

The engine stores strings as UTF-8 (a lone surrogate is kept as its three bytes), and scripts see UTF-16 code units: `length`, indexing, `charCodeAt`, `slice`,
`indexOf`, padding, regular expression positions (`index`, `lastIndex`, `search`, replace offsets), DOM text offsets (`CharacterData`, `splitText`, the
selection of an input) and iteration by code point all work on them. `toUpperCase` / `toLowerCase` (with the special casing and the final sigma rule) and
`normalize` use the tables of `src/Script/UnicodeTables.hpp`, made by `scripts/gen_unicode_text_tables.py`.
A string that is not valid UTF-8 is a *byte string*: every byte is a character 0..255 (the internal convention of `Blob`, `TextDecoder`, `atob` ...; `__bytes_to_string`
and `atob` mark theirs). A byte string that happens to be valid UTF-8 and is not marked is read as text. Use `__text_bytes(str)` for the UTF-8 bytes of a string.


## Wine's Windows tests

`tests/windows/wine` holds Wine's conformance tests for the system DLLs of `src/Windows` (343 programs build; see its README for the build, the licence and what does not
link). They measure how far the DLLs and system services are from real Windows: nearly all of them fail today because the DLLs are stubs (`docs/WindowsUserMode.md`),
and they are meant to be worked through DLL by DLL as the implementations arrive.
