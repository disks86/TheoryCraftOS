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
  (`FunctionCode::library`). Left: the `prototype` of built-in constructors is configurable, module namespace properties are accessors
  (not writable data properties), no strict-only early errors (octal literals, duplicate parameters, `eval`/`arguments` assignment). Also missing:
  `SharedArrayBuffer`/`Atomics` are single-threaded stand-ins (no `wait`/`notify` blocking, `instanceof ArrayBuffer` is true); missing: `Intl` beyond basic formatting, RegExp `v` flag (set notation) / `d` flag /
  duplicate named groups / case folding beyond ASCII (and a non-`u` `.` matches a whole code point, not a code unit) (and `\p` covers the general categories, the main scripts and some binary properties only), `Object.prototype.toString` tag for Date,
  TDZ and `const` only inside functions and blocks (top-level `let`/`const` are globals), duplicate `let` at top level, array destructuring does not close its iterator, sloppy `arguments` mapping, `Symbol.match/split/unscopables`, locale-specific case mapping (`toLocaleUpperCase('tr')`), `new.target`/`Reflect.construct`, tagged template caching, `for…in` integer key order, and a call depth of 500.
- **HTML**: the parser follows the standard (`src/Dom/HtmlBuilder.cpp`: tokenizer states, insertion modes, adoption agency, foster parenting, foreign
  content, templates, quirks mode). Left: the doctype is not a node (`document.doctype`, `publicId`/`systemId`), `DOMParser` with XML, character
  references that stand for two code points, `innerHTML` scripts must not run.
- **CSS**: closed: `getComputedStyle` reports table parts (with `cellpadding`), controls, closed `dialog`, blockified `display`, `em`/`i`, `pre`, `ol`, link colour,
  `sub`/`sup`, `mark`; the properties that used to read `""` (`text-indent`, `white-space` longhands, `word-break`, `overflow-wrap`, `text-overflow`, `quotes`,
  `tab-size`, `list-style-position`, counters, `content`, `grid-*-start/end`, `column-count`, `mix-blend-mode`, `contain`, `accent-color`, `appearance`, `will-change`,
  `border-collapse`); `line-height: normal` and numbers that follow the font size; `text-align: start`; `font-style: oblique <angle>`; `aspect-ratio` as written;
  `transform-origin` and `background-position` keywords; `url()` and gradient colours serialized; `lh`; `all`; `revert`; CSS nesting; logical properties with
  `direction` / `writing-mode`; `:nth-child(… of S)`, `:dir()`, `:read-write` on `contenteditable`; `calc()` needs spaces around `+` and `-`; an empty custom property;
  a `var()` cycle uses its fallback; `rgb()` rounds; `color` keeps `lab()`, `lch()`, `oklab()`, `oklch()`, `color()` and `color-mix()` in their own space and `rgb()`/`hsl()`/`hwb()`/`lab()`… relative colours (`from`) work for plain channel keywords (a `calc()` of them does not).
  Still open: heading sizes, `ul` padding (40px), `blockquote` margin and `small` size (the UA metrics are terminal cells on purpose), `hr` border, other properties than `color` (`background-color`, borders) still read newer colour spaces as sRGB, `revert-layer`, `flow-root` (reads `block`), `@container` conditions (container sizes are
  unknown without layout, so they match), unitless non-zero lengths (accepted as pixels, as in quirks mode)
- **Web APIs**: the constructor of an interface inherits from the constructor of its parent, `AbstractRange`/`StaticRange`, `EventSource`,
  `IDBVersionChangeEvent`, the device events, `DOMImplementation`, `NodeFilter` as an interface object, the legacy element interfaces and the
  constants of `WheelEvent`/`KeyboardEvent`/`DOMException` exist (`finish_prelude()` in `WebPrelude.cpp`, run last). The attributes the engine keeps on the
  objects are mirrored as placeholder accessors on the prototypes, so `'nodeType' in Node.prototype` is true (a read that reaches one gives undefined, a
  write makes the object's own property; they are not enumerable). The SVG DOM is there (`svg_prelude()` in `WebPrelude.cpp`: `SVGElement` and the interface of each element, animated lengths / numbers / strings / enumerations / `viewBox` / `transform`, `getBBox`, `getCTM`, `getTotalLength`, `getPointAtLength`, `isPointInFill`, text methods; text metrics use the 8 x 16 cell, there is no animation and `getScreenCTM` is `getCTM`). Web Audio is there too (`web_audio_prelude()` in `WebPrelude.cpp`: `AudioContext` / `OfflineAudioContext` with a graph rendered 128 frames at a time, `AudioBuffer`, `AudioParam` automation, the oscillator, buffer source, constant source, gain, delay, biquad, IIR, stereo panner, panner, wave shaper, convolver, compressor, analyser, splitter / merger, script processor and audio worklet nodes, `decodeAudioData` through the engine's decoders; the sound of an online `AudioContext` is rendered and handed to `__audio_sink(samples, channels, rate)` when the page environment defines one, and dropped otherwise, there is no output device yet; `HRTF` panning is equal power; the compressor is not bit exact; media element and stream sources are silent). The Web Animations API is there (`animation_prelude()` in `WebPrelude.cpp`: `Animation`, `KeyframeEffect`, `DocumentTimeline`, `element.animate()`, `getAnimations()`, the timing model, keyframe processing, easing, interpolation of numbers / lengths / colours / transforms / lists, composite add and accumulate, `commitStyles()`, `persist()` and automatic removal of replaced animations; the animated values are a layer on top of the cascade (`__set_animated_style`, applied by `derive_style`, over the style attribute and under `!important` rules)). CSS `animation-*` and `transition-*` are parsed but nothing runs them yet (no `CSSAnimation` / `CSSTransition` instances, no `animationstart` / `transitionrun` events), and there are no scroll or view timelines. Still missing: FontFace,
  `DocumentType`/`document.doctype`, form/options collections as interfaces, most methods listed in `missing_members`, and the prototype-level
  accessors for attributes of `Window`.
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

## Strings

The engine stores strings as UTF-8 (a lone surrogate is kept as its three bytes), and scripts see UTF-16 code units: `length`, indexing, `charCodeAt`, `slice`,
`indexOf`, padding, regular expression positions (`index`, `lastIndex`, `search`, replace offsets), DOM text offsets (`CharacterData`, `splitText`, the
selection of an input) and iteration by code point all work on them. `toUpperCase` / `toLowerCase` (with the special casing and the final sigma rule) and
`normalize` use the tables of `src/Script/UnicodeTables.hpp`, made by `scripts/gen_unicode_text_tables.py`.
A string that is not valid UTF-8 is a *byte string*: every byte is a character 0..255 (the internal convention of `Blob`, `TextDecoder`, `atob` ...; `__bytes_to_string`
and `atob` mark theirs). A byte string that happens to be valid UTF-8 and is not marked is read as text. Use `__text_bytes(str)` for the UTF-8 bytes of a string.
