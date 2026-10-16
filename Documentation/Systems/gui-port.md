# GUI - an eepp-derived UI framework (Experimental, parked)

> Status: CURRENT
> Verified: 2026-08-12 @ 12114827
> Track: [[gui-port]]

An EXPERIMENTAL, PARKED alternative UI framework: a fresh `experimental.gui` (`Code/Experimental/GUI`)
built bottom-up on the engine's own infra (VG + Fonts + Core + viewport-input), progressively slicing
eepp's UI design rather than lifting it wholesale. This is a separate module and NOT the engine's UI -
the shipped `foundation.ui` (Sedulous-derived, [[ui-port]]) is the real one; GUI borrows eepp's
VG/Drawable and CSS RECIPES, not its code, and is parked in the Experimental role.

## What is built (bottom-up, on-screen)

The progressive slice reached a working, CSS-driven, on-screen framework:
- **Skeleton + type adapters** (`experimental.gui`), **Transformable + Node**, and the **render seam**
  `DrawContext` over VG + core drawables (there is no immediate-mode GL as in eepp; the seam was
  invented against VG).
- **SceneRoot + coordinator + actions** (`ActionManager`), the platform-agnostic `EventDispatcher`
  (Inject* API), and the `experimental.gui.shell` input bridge (`GuiInputBridge`, fed by the shell).
- **Text/Font** (`Text` over `foundation.fonts`), **UINode / UIWidget** chrome (padding, control state).
- **A CSS engine, essentially complete**: parsing + `StyleManager` (StyleSheet + MediaContext +
  per-widget resolved-style cache), `:hover` re-resolution driving transitions through the
  ActionManager live (`hovering a widget re-resolves :hover and drives its transition`), transitions
  (`Styling/Transition.cppm`). The GUISandbox drives a Button through the dispatcher (click, control
  state) styled via CSS, on-screen through VG -> VGRenderer -> RHI, input from a fullscreen
  `InputSurface`.

Per the track memory: ~16 widgets, 3 layouts, popups, and the CSS engine landed before parking.

## Why progressive-slice (not wholesale-lift)

A wholesale lift of eepp's `ui` (~118k LOC) is a trap: its dependency cone is most of the engine
(graphics/system/scene/core/window/math), there is NO renderer seam (it draws through immediate-mode GL /
`GlobalBatchRenderer`), and it clashes on every constraint (STL + exceptions + RTTI + raw-pointer
ownership vs named modules / `-fno-exceptions` / `-fno-rtti` / own containers / `Object` reflection). The
slice builds green feedback the whole way.

## Status: parked

The framework is parked in the Experimental role. Remaining (if resumed): heavy widgets (CodeEditor,
HTML/Markdown, Terminal) opt-in; keyboard/text input breadth; markup + tools (XML/CSS layout loading on
`foundation.xml`); `background-image -> Drawable*` / `font-family -> Font*` resource resolution + CSS
hot-reload; and eepp's HarfBuzz + bidi (its unique strength) deferred until rich-text widgets need it.

---

The eepp reference analysis (the dependency-cone census, the namespace map, the wholesale-lift trap in
full) is in `Documentation/Archive/gui-port-design-history.md`.
