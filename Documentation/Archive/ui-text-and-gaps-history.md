# UI gaps: real text shaping, inline runs, and what else we are missing

> ARCHIVED 2026-11-02, obsolete: its subject was replaced or removed. Non-authoritative: the
> original spec, kept as the record of what was built and why; the code and the tests are the truth.

> STATUS: PROPOSED 2026-09-23; its text track (P0-P2) is scheduled as the F line of
> `vg-and-gui-lab.md` (2026-10-15), re-cut with inline runs ahead of bidi, and its gap 7's font
> effects as F4. The rest of the gap list stands here. Sized L for the text track (P0-P2), with the
> remaining gaps named and sized but not specified to phase depth. Origin: a user ask ("compare
> MewUI and RmlUi against our UI stack and tell me where we are lacking"), answered after
> reading RmlUi (`~/Dev/CPP/RmlUi`, 54k lines of core) and MewUI (`~/Dev/CS/MewUI`, 227k lines
> of C#) against this tree. This spec records the comparison so it does not have to be redone,
> then commissions the top item. Read CONVENTIONS.md first.

## Goal

Our UI can lay out a game or an editor, but it cannot render most of the world's writing
systems, and it cannot put two different formats in one paragraph. The text track fixes both,
in that order, because they are one problem: a shaper without runs buys little, and runs
without a shaper are a prettier version of the same limitation.

Not goals: becoming a browser. Floats and tables are CSS-document features and are explicitly
declined below; so is an accessibility tree, which is real but is its own spec.

## What exists (checked, cited per the Specs rule)

- **The shaper seam is already there.** `Foundation/Fonts/Interfaces.cppm:73`, `ITextShaper`,
  with `FontManager::SetShaperFactory` (`FontManager.cppm:46`) selecting the implementation and
  `NullFontService` as the headless one. A second backend does not need a new abstraction.
- **But its DATA MODEL assumes one codepoint is one glyph.** `GlyphPosition`
  (`Foundation/Fonts/Types.cppm:74`) is `{stringIndex, codepoint, x, y, advance, glyphInfo}`.
  There is no glyph id distinct from the codepoint, no cluster (a shaped run maps N codepoints
  to M glyphs, neither 1:1 nor order-preserving), and no per-glyph offset distinct from the
  advance. This struct, not the interface, is the thing real shaping breaks.
- **`ITextShaper` is single-font and single-run by signature**: every entry point takes one
  `IFont&` and one `StringView`. No font fallback within a string, no script or language tag,
  no bidi level.
- **The current shaper is stb_truetype with legacy kerning.**
  `Fonts.TrueType/TrueTypeTextShaper.cppm:92-137` advances codepoint by codepoint and adds
  `font.GetKerning(prev, cp)`, which is `stbtt_GetCodepointKernAdvance` (`TrueTypeFont.cppm`).
  No GSUB/GPOS, so no ligatures, no contextual forms, no mark attachment.
- **`Label` is a single run.** `Foundation/UI/Controls/Label.cppm:37-44`: one `Property<String>
  Text`, one `FontSize`, one `FontFamily`, one `TextColor`. Its measure cache is keyed on
  `(text, family, fontSize, maxWidth, wrap)` (line 113), which is the same assumption.
- **`GlyphPosition` has 21 consumers** across `Fonts`, `Fonts.Coverage`, `Fonts.TrueType`,
  `VG/Context.cppm`, and `UI/Controls/Label.cppm`, `UI/Controls/NumericField.cppm`,
  `UI/Editing/TextEditingBehavior.cppm`, `UI/Editing/ITextEditHost.cppm`. Hit testing and
  cursor positioning (`ITextShaper::HitTest`, `GetCursorPosition`) are the delicate ones: they
  translate between string offsets and x positions, which is exactly what clusters change.
  Nothing outside the fonts modules reads `.codepoint` directly (grep: 0 hits in `UI/` and
  `Fonts/`), so the field can change meaning without a wide edit.
- **The cascade is real and does not need revisiting.** `ui-layout-and-style-model.md` P0-P4 are
  BUILT: ordered cascade, selector chains, `inherit`/`initial`, custom properties and `var()`,
  the box model, transitions, flex with wrapping. This spec does not touch it.

## Prior art (both read, 2026-09-23)

**RmlUi** (`Source/Core`, 54k lines) is an HTML/CSS renderer for games:

- Every CSS formatting context: `BlockFormattingContext`, `InlineContainer` / `InlineBox` /
  `InlineLevelBox` / `LineBox`, `FlexFormattingContext`, `TableFormattingContext`, and floats
  through `FloatedBoxSpace` (all in `Source/Core/Layout/`).
- Media queries (`StyleSheetContainer`), a specificity-ordered cascade (`StyleSheetNode`).
- MVVM data binding: `DataModel`, `DataView`, `DataController`, `DataExpression`.
- Effects: `FilterBlur`, `FilterDropShadow`, `ConvolutionFilter`, and font effects
  (`FontEffectOutline`, `FontEffectGlow`, `FontEffectShadow`, `FontEffectBlur`).
- 3D transforms, an interactive `Source/Debugger`, and Lua / SVG / Lottie plugins.
- Its core font engine is FreeType; HarfBuzz is a sample (`Samples/basic/harfbuzz`), not core.
  So RmlUi is *not* ahead of us on shaping out of the box - it shows the integration shape.

**MewUI** (`src/MewUI`, 227k lines of C#) is a WPF-shaped desktop framework:

- WPF measure/arrange panels (`Canvas`, `DockPanel`, `Grid`, `StackPanel`, `UniformGrid`,
  `WrapPanel`, `SplitPanel`); no CSS layout at all.
- `Styling/` is Style + Setter + `StateTrigger` / `ElementTrigger` + control templates.
- Full path-based binding (`Binding/`): `BindingPath`, `BindingPathObserver`, `BindingMode`,
  converters, `ValidationError`, `BindingDiagnostics`.
- `Text/` is a managed engine with real runs: `ManagedTextRunLines`, `ManagedTextFragments`,
  `MarkupTextParser`.
- **Real shaping on every desktop platform**: HarfBuzz (`Shared/Native/Linux/HarfBuzz`),
  DirectWrite (`Shared/Native/Win32/DirectWrite/DWriteGlyphRunExtractor`), Fontconfig, FreeType.
- `Diagnostics/DevTools/`: `DebugVisualTreeWindow`, `DebugPropertyPanel`, `DebugInspectorOverlay`,
  a profiler with markers, and `HotReload/`.

## The gap list, ranked

Ranked by what it unlocks, not by effort.

1. **Text shaping** - no GSUB/GPOS, no bidi, no complex scripts (Arabic, Indic, Thai), no
   ligatures. Already named as out of scope in `ui-layout-and-style-model.md`. **This spec.**
2. **Inline runs** - no mixed format within a paragraph, so no bolded word, no coloured name, no
   inline icon. **This spec.**
2b. **Parity testing** - our layout expectations are hand-derived, so a misreading of CSS
   produces a test that agrees with the bug. Lunarsong measures Chrome instead. Split out to
   `ui-browser-parity-tests.md`; its P3 (text geometry) depends on P0-P1 here.
3. **Dev tooling** - `UI/Debug/UIDebugOverlay` draws bounds, padding, margin, hit target and
   focus path. It is not an inspector: no tree, no live property editing, no profiler. Both
   comparators have one. Productivity, not capability. **Own spec, recommended next.**
4. **Data binding** - `Property<T>::BindTo`/`BindTwoWay` is property-to-property; the list
   adapters cover collections. No path binding to a model, no converters, no validation. The
   layout spec declined this deliberately ("our adapters stay"); revisit only if the editor's
   inspector work starts wanting it.
5. **Accessibility tree** - absent. Neither comparator is strong here either, so this is an
   absolute gap rather than a competitive one. Own spec.
6. **Media queries**, **sibling (`+`, `~`) and attribute (`[attr=value]`) selectors**,
   **stylesheet hot reload** - small, independent, schedule when a consumer asks.
7. **Filters and font effects** - RmlUi has blur, drop-shadow, convolution and text
   outline/glow/shadow; we have box-shadow and gradients. Worth it when the UI art asks.

**Declined**: floats and tables. They exist in RmlUi because it renders documents. A game UI
and an editor want flex, grid and dock, which we have. Adding two more formatting contexts is
real cost against no demand.

## Decision 1 - the glyph model changes before the shaper does

`GlyphPosition` grows a glyph id and a cluster, and `codepoint` stops being the glyph's
identity:

```
GlyphPosition  { stringIndex, codepoint, x, y, advance, glyphInfo }
            -> { cluster, glyphId, x, y, xOffset, yOffset, advance, glyphInfo }
```

`cluster` is the byte offset into the source string that this glyph belongs to; several glyphs
may share one, and one glyph may cover several codepoints. `xOffset`/`yOffset` are the
positioning deltas GPOS produces, which the current model cannot express at all.

This lands FIRST, with the existing stb shaper filling `cluster = stringIndex` and
`glyphId = codepoint`, so every consumer is migrated and green before a second backend exists.
Hit testing and cursor movement are rewritten against clusters in this phase - that is the
actual work, and doing it under the old shaper keeps it verifiable.

Rejected: a parallel "shaped text" type beside the old one. Two models means every consumer
picks, and the editor and the game would drift onto different ones.

## Decision 2 - HarfBuzz is a backend behind the existing factory, not a dependency of Fonts

`SetShaperFactory` already selects the implementation. A `Fonts.HarfBuzz` module registers a
shaper the same way `Fonts.TrueType` does; the Fonts module keeps no HarfBuzz symbols and the
null service keeps working headless. `ITextShaper` widens to carry a script, a language and a
direction, and to take a font *list* rather than one font, so fallback happens inside the
shaper where the itemizer lives.

Following RmlUi's shape here (theirs is a sample precisely because the engine does not depend
on it) rather than MewUI's (which binds three platform stacks - DirectWrite, HarfBuzz,
Fontconfig - and pays for all of them).

## Decision 3 - a run is a styled span, and Label gets a sibling rather than a mode

`Label` keeps its single-run contract and its measure cache exactly as they are. Rich text is a
new control over a `TextDocument` of runs `{text, family, size, colour, weight}` plus inline
objects, laid out by an inline formatter that stacks runs into lines.

Rejected: making `Label` polymorphic on whether its text has markup. Its measure cache is keyed
on one family and one size (`Label.cppm:113`); making that conditional is how a fast path
becomes a slow one nobody notices.

## Phases

- **P0 - the glyph model**: `GlyphPosition` gains cluster / glyphId / offsets; the stb shaper
  fills them degenerately; hit testing, cursor movement and every one of the 21 consumers move
  to clusters. Acceptance: every existing text test green with no visual change, and a test
  that pins cluster behaviour for a 1:1 script so the semantics are written down before a
  shaper can vary them.
- **P1 - the HarfBuzz backend**: `Fonts.HarfBuzz`, a widened `ITextShaper` (script, language,
  direction, font list), itemization and fallback. Acceptance: an Arabic string shapes with
  joined forms and its glyph count differs from its codepoint count; a Latin string with an
  `fi` ligature produces one glyph for two codepoints with a shared cluster; both hit-test to
  the right string offsets; the stb backend still passes the P0 suite.
- **P2 - bidi and inline runs**: the UBA for mixed-direction paragraphs, then the run model and
  the rich-text control. Acceptance: an RTL paragraph with embedded Latin lays out and
  hit-tests correctly; a paragraph mixing two sizes and three colours measures and wraps
  correctly; an inline icon sits on the baseline.

## Gotchas

- **Cluster maths is where the bugs live.** Cursor left/right is not glyph +/-1 once clusters
  exist; selection ranges are byte ranges that may not fall on glyph boundaries. Pin this with
  tests in P0, under the old shaper, before anything can shape differently.
- **The measure cache keys are part of the contract.** `Label`'s cache assumes one family and
  one size; a run model must not quietly reuse it.
- **Atlas pressure.** Complex scripts and fallback fonts multiply the glyph set, and our atlas
  bakers (`Fonts.Coverage.Baker`, `Fonts.DistanceField.Baker`) bake a coverage set ahead of
  time. Shaping by glyph id rather than codepoint changes what "coverage" means - decide
  whether the baker takes glyph ids or stays codepoint-driven with a runtime fallback path.
- **Do not let the editor and the game diverge.** Both draw through `VG/Context.cppm`, which is
  a `GlyphPosition` consumer; a partial migration would leave one of them on the old model.
- **The Beef port follows one for one.** Its `Sedulous.Fonts.TrueType` and `Sedulous.UI` mirror
  these files exactly (`TrueTypeTextShaper.bf` has the same kern loop, `Label.bf` the same eight
  properties), so every seam named here exists there under the same name.
