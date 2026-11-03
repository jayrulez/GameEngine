# UI parity tests: measure the reference, do not derive the expectation

> ARCHIVED 2026-11-02, obsolete: its subject was replaced or removed. Non-authoritative: the
> original spec, kept as the record of what was built and why; the code and the tests are the truth.

> STATUS: PROPOSED 2026-09-23, not scheduled. Sized M for P0-P1, L with the later families.
> Origin: reading Lunarsong's UI module (`~/Dev/CPP/Lunarsong/Engine/Modules/UI`) while
> answering "compare it against our UI stack". Its parity suites are the best idea in any of
> the three UI codebases surveyed (RmlUi, MewUI, Lunarsong), and they address a weakness in our
> shipped layout engine that our own tests cannot see. Read CONVENTIONS.md first.

## Goal

We claim CSS semantics. Our tests assert what we believed those semantics were when we wrote
the code, so a misreading of the spec produces a test that agrees with the bug. Parity tests
replace derived expectations with MEASURED ones: the number in the test comes from a browser
laying out the same markup, and a divergence is either fixed or pinned in writing as deliberate.

Not goals: rendering-level image diffing (we are not trying to match Chrome's rasterizer),
becoming a browser, or testing anything the engine does not claim to implement.

## What exists (checked, cited per the Specs rule)

- **54 UI test files, 15,135 lines** in `Code/Foundation/UI.Tests/` — a real suite covering
  cascade, box model, flex wrap, controls, input, markup, drawables and editing.
- **Every expectation in it is hand-derived.** `FlexWrapTests.cpp:88` is the pattern: a comment
  reasoning the answer out (`// 250 wide: two 100s per line -> [a b] [c d] [e]`) above
  `CHECK(items[0]->Bounds.x == doctest::Approx(0))`. The comment is the authority, and the
  comment is us.
- **No golden, snapshot or parity infrastructure of any kind** (`ls Code/Foundation/UI.Tests |
  grep -iE "golden|snapshot|parity|baseline"` returns nothing).
- **The layout and style model shipped P0-P4** (`ui-layout-and-style-model.md`): ordered
  cascade, selector chains, `inherit`/`initial`, custom properties and `var()`, the box model,
  transitions, flex with wrapping, `align-content`, gaps and `flex-basis`. All of it
  hand-written against our reading of CSS, which is precisely the surface this spec tests.
- **Our box model is border-box**, stated in `UI/Core/View.cppm:121` ("Border is
  layout-participating chrome (border-box): MeasuredSize = content + padding + border") and
  again in `Controls/Panel.cppm:91` and `Drawing/RoundedRectDrawable.cppm:56`. This matters:
  the fixture reset has to match it or the two sides answer different questions.
- **The spec already records known divergences** from CSS: `flex-shrink` unread, no `order`, no
  `wrap-reverse`, no shorthands, quadratic easings rather than cubic-beziers, `flex-basis: auto`
  with grow meaning 0 rather than content. These are the first things parity will find, and
  they must be pinned as deliberate rather than "fixed" by accident.

## Prior art: Lunarsong's parity suites (read 2026-09-23)

`Engine/Modules/UI/Tests/` is 150 files and 67,675 lines — more test code than our entire UI
core. Eleven of those files are parity suites: `FlexWrapGapChromeParityTests`,
`FlexJustifyAlignParityTests`, `FlexGrowShrinkBasisParityTests`, `FlexDefaultsParityTests`,
`TextGeometryChromeParityTests`, `TextPenPlacementParityTests`, `LineBoxDevicePixelParityTests`,
`CascadeShareParityTests`, `UIEncodedBlendParityTests`, `TextMaskGammaGoldenTests`, and the
`BaselineChromeParityProbe` measurement harness.

Their method, from the header of `FlexWrapGapChromeParityTests.cpp`, is worth copying almost
whole:

- **Ground truth is real Chrome headless**, invoked per fixture with
  `--headless=new --disable-gpu --no-sandbox --no-first-run --user-data-dir=<tmp>
  --force-device-scale-factor=1 --virtual-time-budget=3000 --dump-dom file:///<tmp>/<fixture>.html`.
- **One page per fixture**, not one page with prefixed selectors, so the CSS text in the test is
  byte-identical to what Chrome parsed — no scoping selector, no duplicate ids changing matching.
- **The reset matches the engine's box model.** Theirs is
  `* { box-sizing: border-box; margin: 0; padding: 0; border: 0 solid transparent }`, chosen
  because Yoga's width is always the border box, "so a content-box fixture would be comparing
  two different questions".
- **Fixtures carry no text** in the layout families, keeping them clear of a known text-ink
  rounding difference — confounds are removed rather than tolerated.
- **Every property is restated explicitly** on every container and item, because Yoga's defaults
  are not CSS's (column vs row, shrink 0 vs 1): "a fixture that leans on either side's default
  is testing the default, not the feature".
- **A scale-invariance cross-check.** The same 20 pages measured at
  `--force-device-scale-factor=2` returned byte-identical CSS-px rects, 0 differing fields over
  628, with `devicePixelRatio` read back as 1 and 2 to prove the flag took effect. That
  invariance is what makes their `contentScale 2.0` assertions well-posed.
- **Rects are compared relative to the container's border-box origin**, which is exactly what
  differences of `getBoundingClientRect` give on the Chrome side.
- **Deliberate divergences are pinned in the same file, marked in capitals**, with the reason.
- **An isolated fixture** (`Tests/IsolatedUIFixture.h`): headless device, exactly one
  stylesheet — the string the test passed — no asset manager, no project mount, no runtime style
  injection, no input delivered, so nothing is hovered or focused unless the test does it. "A
  number read back here is attributable to the CSS the test wrote and to nothing else."
- **The capture pages and diff scripts are checked in**, under `Tools/ai/<topic>/`
  (`chrome_fixture.html`, `chrome_diff.ps1`), so a measurement can be re-derived.
- **The expensive probe is DISABLED by default** and documented with the exact command to run
  it: 58 fixtures per scale, about two minutes, asserting nothing. Its output is what the real
  expectations were derived from.

**One difference that raises the value for us.** Lunarsong delegates layout to Yoga and CSS to
Lexbor — both reference-grade implementations. Their parity tests mostly confirm their
*integration*. We hand-wrote both, so ours would be testing the implementation itself. We have
more to gain and more to find.

## Decision 1 - an isolated fixture is the prerequisite, not a nicety

Before any parity number is worth recording, a test must be able to say what produced it.
`UI.Tests` today builds a `UIContext`, a root and views directly (`FlexWrapTests.cpp:108-115`),
which is close, but a parity fixture must additionally guarantee: exactly one stylesheet (the
string the test passed), no theme or UA sheet unless the test asks for one, no input delivered,
and a stated content scale. Anything ambient is a number nobody can attribute.

This lands first and is useful on its own — it is a better harness for the existing tests too.

## Decision 2 - Chrome headless is the reference, and the capture is checked in

Chrome, invoked exactly as Lunarsong does, one page per fixture. Not a hand-read of the CSS
spec, and not a second engine: the point is a reference implementation everyone can re-run.

The generated pages and the capture script go in the repo (`Tools/ui-parity/`), so any number in
a test can be re-derived by the next person without reconstructing the method from the test's
comments. A measured number with no reproducible provenance is a magic constant.

Rejected: comparing against Firefox or WebKit as well. Where browsers disagree, the disagreement
is usually in a corner we do not implement; one reference is enough and two is a tie-break
nobody has time to arbitrate.

Rejected: image diffing. We are testing layout and geometry semantics, not rasterization, and
our renderer is an SDF/VG path that will never match Chrome's pixels.

## Decision 3 - the fixture reset states our box model, and every property is restated

The reset is `* { box-sizing: border-box; margin: 0; padding: 0; border: 0 solid transparent }`,
matching `View.cppm:121`. A content-box fixture would compare two different questions.

Every container restates direction, wrap, justify, align-items, align-content and both gaps;
every item restates grow, shrink and basis. Our defaults are not CSS's — the layout spec records
`flex-basis: auto` with grow meaning 0 for us and content + share in CSS — so a fixture leaning
on a default tests the default, not the feature.

Layout-family fixtures carry no text until the text work lands, so glyph metrics are not a
confound in a test about boxes.

## Decision 4 - a divergence is fixed or pinned, never left ambiguous

Every measured difference resolves one of two ways, in the same commit:

- **Fixed**, when we simply had CSS wrong.
- **Pinned**, when the difference is deliberate: the test asserts OUR number, marked in capitals
  with the reason and a pointer to where the decision was made. The known list from
  `ui-layout-and-style-model.md` — `flex-shrink` unread, no `order`, no `wrap-reverse`,
  quadratic easings, our `flex-basis: auto` semantics — all land here on day one.

What must not happen is a parity suite that skips the cases we fail. A skipped case is a
divergence nobody wrote down.

## Phases

- **P0 - the fixture and the capture tool**: the isolated fixture, `Tools/ui-parity/` (page
  generator + Chrome invocation + a diff that prints per-field differences), and the first
  family, flex justify / align-items, as proof. Acceptance: a fixture's numbers can be
  re-derived from a clean checkout with one documented command; the first family is green or
  its divergences are pinned with reasons.
- **P1 - the flex families**: wrap, `align-content`, both gaps, grow / shrink / basis, and the
  defaults family. Acceptance: every case green or pinned; the scale-invariance cross-check at
  `--force-device-scale-factor=2` returns identical logical rects, which also validates our own
  content-scale handling.
- **P2 - box model and painting geometry**: padding, border, min/max, `position: absolute`
  containing blocks, overflow. Acceptance: as P1.
- **P3 - text geometry** (AFTER `ui-text-and-gaps.md` P0-P1 land; pointless before, since our
  shaper cannot produce comparable numbers): line box height, baseline placement, pen
  positions. Lunarsong's `BaselineChromeParityProbe` is the model — a measurement harness that
  asserts nothing, from which real expectations are derived.

## Gotchas

- **Confounds must be removed, not tolerated.** Lunarsong keeps text out of layout fixtures
  because of a known text-ink rounding difference. A parity test that is "close enough" teaches
  the next person that the suite is approximate, and then it stops being run.
- **One page per fixture.** Prefixed selectors in a shared page change what the CSS text is, and
  the whole value is that the test's CSS is byte-identical to the browser's.
- **Our defaults are the trap.** More of our surface diverges from CSS by choice than Lunarsong's
  does, because they inherited Yoga's behaviour and we wrote ours. Expect the first run to
  produce a long pinned list, and expect reviewing that list to be the real work of P1.
- **Do not let the probe rot.** The expensive measurement harness should be disabled by default
  with its exact command in the header, as theirs is — a two-minute test in every run gets
  deleted, and an undocumented one gets forgotten.
- **Beef port**: `Sedulous.UI.Tests` mirrors these files, and the parity NUMBERS are
  language-neutral. Capture once, assert in both trees; a divergence between the two ports
  against the same measured table is itself a useful signal.
