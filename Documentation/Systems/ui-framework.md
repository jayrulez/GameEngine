# UI Framework (foundation.ui core)

> Status: CURRENT
> Verified: 2026-08-16 @ 993d57d2
> Track: ui-core-audit.md (Backlog) / Specs/ui-box-model.md / Specs/ui-theme-migration.md

The retained-mode UI framework itself - box model, focus, styling/theming, and the frame
pipeline. Ported from Sedulous.UI and then taken "good to great" by the 2026-08 core audit
(P0 focus correctness, P1 perf caches + damage gate, P2 border-box model, theme migration
to `.sss` text). The runtime tiers that HOST this framework (game canvases, editor UIHost)
are documented in `game-ui.md`; this doc covers the framework contract.

## Modules

- **`foundation.ui`** (`Code/Foundation/UI`) - the core: `View`/`ViewGroup`, controls,
  layouts, styling (`Styling/`), popups/overlays, drag-drop, virtualization, markup.
- **`foundation.ui.toolkit`** (`Code/Foundation/UI.Toolkit`) - editor-grade widgets:
  docking, menu/tool/status bars, property grid, pickers, node graph, curve/gradient
  canvases, the code editor.
- **`foundation.ui.runtime`** - `UIHost`: window attachment, per-frame Update/Draw, the
  damage gate, theme-icon baking, DPI/UI-scale.
- Sibling libs: `.viewport` (3D-in-UI), `.shell` (input bridge), `.application`,
  `.vfs`, `.resource` + `ui.pipeline` (cooked documents/themes - see `game-ui.md`).

## Box model (border-box; Specs/ui-box-model.md)

- `BoxMetrics{Margin, Padding, Border}` resolved ONCE per view by
  `View::ResolveBoxMetrics()`: padding = max of the ViewGroup padding field
  (`OwnPaddingField()`), styled padding (or the control's `DefaultStylePadding()` when no
  rule sets one), and the background drawable's `DrawablePadding()`; border from
  `StyleProperty::BorderWidth`.
- `View::Measure` is a template method (margin deflate + `Fixed` specs at LOGICAL dpi);
  controls implement `OnMeasureContent` (sentinel `Float2{-1,-1}` falls back to legacy
  `OnMeasure`). `Match` stays PARENT-side - a base-side Match would defeat Flex's
  cross-axis demotion.
- `View::Layout` receives the MARGIN BOX, insets once, and rounds each edge independently
  to the device grid (a 1.25x viewport emits integral device pixels).
- Units: `Dp` = logical identity, `Pt` = 96/72 logical, `Px` = physical/scale.
- `BoxConstraints::IsBounded` / `BoundedMaxWidth/Height(fallback)` are THE unbounded
  test; fill-type leaves carry bounded defaults so kFloatMax never leaks into sizes.

## Focus (audit P0)

- `FocusSource{Programmatic, Pointer, Keyboard}`: focus is RETAINED regardless of source;
  the ring draws only for keyboard focus (`View::IsFocusVisible()`; text inputs always).
- Popup/modal focus save+restore lives in `PopupEntry`
  (`FocusManager::SaveAndClearFocus`/`RestoreFocus`) - validated restore, no
  cross-restore on out-of-order closes; restore carries the original source.
- `GetFocusRoot()` scopes Tab to the topmost focus-taking popup: modals trap the tab
  ring; `Dialog::Show` sets initial Programmatic focus so Escape works immediately.
- Tree mutations during the DRAW phase are `DIAGNOSTIC_ASSERT`ed (layout-phase mutations
  are legal - virtualization realizes rows in OnLayout). `Queue*` lambdas capture
  `RefPtr` so pending actions co-own their views.

## Styling + theming (Specs/ui-theme-migration.md)

Themes are `.sss` text. The four built-in looks (Dark, Light, RoundedDark, plus the
toolkit fragments) live as in-tree sheets embedded at configure time
(`Styling/Themes/*.sss` -> `EmbeddedThemes`, `UI.Toolkit/Themes/*.sss` ->
`EmbeddedToolkitThemes`; CMake `file(READ)` + `configure_file` + `CONFIGURE_DEPENDS`).
`DarkTheme::Create(palette)` = `SetPalette + Load(embedded) + ApplyExtensions`;
`ToolkitThemeExtension::Apply` parses the dark or light fragment (predicate:
`palette.Background.r < 0.5`) and `MergeFrom`s it into the theme sheet. The legacy C++
rule builders survive ONLY as a parse-failure belt until visual sign-off retires them.

**Design system** (documented at the top of each sheet): type ramp 16 default / 14 text
inputs + expander headers / 12 compact chrome (buttons, combos, tabs, dock tabs); spacing
scale {2,4,6,8,12,16} (button pad `8 12`, input pad `4 6`); geometry is per-theme
identity (flat radius 0 / rounded 6); EVERY color derives from the 11 palette variables
(`$primary, $primary-accent, $background, $surface, $surface-bright, $border, $text,
$text-dim, $error, $success, $warning`).

**SSS essentials**: `/* */` comments ONLY; selector grammar `Type.class:state::part:state`
(single compound - NO descendant selectors); 2-value `padding` is CSS order (vertical
horizontal), the opposite of C++ `Thickness{h,v}`; drawable factories `color()
rounded-rect(radius=a b c d) state-colors() state-rounded() svg(name[,tint=...])
image(name[,tint=...][,radius=a b c d]) nine-slice() gradient() layer() inset()`; color functions `lighten darken alpha
mix hover pressed disabled focused` (the state ones delegate to `Palette::Compute*`);
`background:` ALWAYS builds a drawable while `background-color:` stores a raw Color for
`ResolveStyleColor` consumers (ToastCard, the canvases, ModalBackdrop, DragAdorner);
semantic status properties `success-color / warning-color / error-color` map to the
palette on the View rule of every sheet (toasts, code-editor gutter markers).

**Resolution semantics + gotchas**:
- Type rules match SUBCLASSES (`IsDerivedFrom`), so `View { font-size: 16 }` reaches
  every control - a control that resolves `FontSize` needs an explicit rule when its
  chrome wants a different size (the dock-header/toolbar/breadcrumb 12s are regression-
  gated in the sheet tests).
- Resolution is PER-VIEW: container rules cannot style child views (StatusBar section
  labels, toolbar button labels). Children of unregistered types resolve via an ancestor
  (`Cast<Toolbar>(Parent)->ResolveStyleFloat(...)`) or stay code-defaulted.
- A `.sss` selector for an UNREGISTERED type silently matches nothing -
  `UITypeRegistry::RegisterBuiltins` (core) and `RegisterToolkitTypes()` (16 types,
  tripwire `kToolkitStyleTypeCount`) must cover every styled type;
  `StyleSheetLoader::Load` self-registers builtins (P1 finding - hosts that skipped
  startup registration used to get silently null selectors).
- `svg(name)` resolves through `ThemeIconSet` (10 shared baked glyphs + tinted-variant
  cache); UIHost bakes them at attach and rebakes on scale change.

**Tests**: `ThemeSheetTests` (UI.Tests) + `ToolkitThemeSheetTests` (UI.Toolkit.Tests)
gate the design system per sheet (ramp values, icon vocabulary resolves, raw-color
contracts, belt-does-not-engage). The earlier rule-for-rule parity gates retired when
the consistency pass deliberately re-authored the sheets.

## Frame pipeline (audit P1)

- **Damage gate** (UIHost): a frame-scoped one-bit decision - clean frames skip
  `UpdateRootView` + `Clear/DrawRootView` and re-encode the RETAINED VG batch. Damage
  producers: hover enter/leave, animations + caret (`BeginFrame` marks when
  `ActiveCount() > 0 || WantsTextInput()`), drag adorner, ListView long-press
  self-chain, ViewportView live-3D self-chain; resize/DPI is structural auto-damage.
  Escape hatch `SetDamageGatingEnabled(false)` + `FramesDrawn/FramesSkipped` counters.
  The game `UISubsystem` path is ungated.
- **P1a caches** (all value-keyed - NEVER font pointers): Label shaped-text (measure +
  draw), ComboBox max-item width, TabView title widths; ListView's rebind branch is
  gone.

## Open items

UA default sheet + state-ladder cleanup (theme-migration P4); legacy theme bodies +
`ApplyLegacyForParity` deletion after user visual sign-off; descendant selectors or
style inheritance for child-label text; CodeEditView syntax-palette property family;
ShapedTextBlock + Event/Property connection tokens (audit P4).
