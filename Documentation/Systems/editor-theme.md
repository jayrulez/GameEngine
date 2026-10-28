# The editor's theme: the token reference

> Status: CURRENT
> Verified: 2026-10-28

The editor's look is written in one place: its sheet, `Data/Editor/editor.css`, over
`Data/Editor/tokens.css` and one of the density sheets (`Data/Editor/density-*.css`). Every rule in
the editor's sheet reads a token, never a literal, so changing a token changes everything that
reads it. This page lists every token, the values it takes in the dark and light schemes, and how
a user overrides any of them. The design behind it is
[Specs/editor-gui.md](../Specs/editor-gui.md), section 3.1.

A test (`Editor.App.Tests`, EditorThemeDocTests.cpp) fails on a token the sheets declare that this
page does not name, so the two stay in step.

## How the sheet is put together

The editor builds its sheet in this order, so a later part wins over an earlier one:

1. `editor.css`, which imports `tokens.css` (which imports the Compact density).
2. The chosen density's sheet (Preferences > Density: Compact, Default or Comfortable).
3. A `:root` block of the Preferences inputs: `--accent-input`, the accent chosen.
4. The user's overrides, `theme/overrides.css` in the user's data folder, if there is one.

The scheme (Preferences > Theme: Dark or Light) and less motion (Preferences > Motion: Reduce
motion) go through the GUI's media queries, `prefers-color-scheme` and `prefers-reduced-motion`.

## Overriding the theme

Write `overrides.css` in the `theme` folder of the user's data folder
(`~/.local/share/gameengine/theme/overrides.css` on Linux). The editor makes the folder when it
opens, sweeps it about once a second, and applies the sheet again when the file is added, changed
or removed; the Console says when it applied. The file is ordinary CSS, applied last:

```css
/* A warmer dark theme and a green accent. */
:root {
    --surface: #1f1d1b;
    --surface-raised: #272421;
    --accent-input: #2f9f5f;
}

/* Only in the light scheme. */
@media (prefers-color-scheme: light) {
    :root { --surface: #f6f3ee; }
}

/* Any rule, too: the Console's lines in the mono font. */
.log-text { font-family: var(--mono-font); }
```

Set the semantic tokens (layer 2) rather than the GUI's (layer 3): the GUI's tokens are set from the
semantic ones, so one change to `--surface` reaches every widget that sits on it. An
`--accent-input` here beats the accent chosen in Preferences.

## Layer 1: the inputs

| Token | Default | What it is |
|---|---|---|
| `--accent-input` | `#2f6fdf` (azure) | The accent's fill, behind white text. Preferences sets it. |
| `--radius-input` | `5px` | The corner radius; `--radius-small` and `--radius-large` follow it. |
| `--ui-font` | `"Inter"` | The interface's font. |
| `--mono-font` | `"JetBrains Mono"` | Code and numbers that line up. |

## Layer 2: the semantic tokens

The dark values are the defaults; the light ones are set under `prefers-color-scheme: light`.
"same" means the light scheme keeps the dark value.

### Surfaces, elements and lines

The chrome frames the window (the menu bar, the tab strips, the status bar); panels sit on
`--surface`; headers and bars on `--surface-raised`; popups and dialogs on `--surface-overlay`;
fields on `--surface-sunken`. Elements are the fill of what you press or type into; ghost elements
(a bar's buttons) have no fill at rest.

| Token | Dark | Light |
|---|---|---|
| `--chrome` | `#15171b` | `#e4e7ec` |
| `--surface-sunken` | `#111316` | `#ffffff` |
| `--surface` | `#1b1e23` | `#f3f4f6` |
| `--surface-raised` | `#22252b` | `#eceef1` |
| `--surface-overlay` | `#282c33` | `#ffffff` |
| `--element` | `#2a2e36` | `#ffffff` |
| `--element-hover` | `#333842` | `#e6e9ee` |
| `--element-active` | `#3c424d` | `#d9dde4` |
| `--element-disabled` | `#23262c` | `#eef0f3` |
| `--ghost-hover` | `rgba(255, 255, 255, 0.07)` | `rgba(0, 0, 0, 0.06)` |
| `--ghost-active` | `rgba(255, 255, 255, 0.12)` | `rgba(0, 0, 0, 0.11)` |
| `--element-on-overlay` | `#383d47` | `#e6e9ee` |
| `--element-on-overlay-hover` | `#434956` | `#dce0e6` |
| `--element-on-overlay-active` | `#4d5461` | `#d0d5dc` |
| `--border` | `#30343c` | `#cfd4db` |
| `--border-strong` | `#454b56` | `#aab1bc` |
| `--separator` | `#282b31` | `#dfe2e7` |

### Text and icons

| Token | Dark | Light |
|---|---|---|
| `--text` | `#d9dde4` | `#1e2228` |
| `--text-strong` | `#f3f5f8` | `#0b0d10` |
| `--text-dim` | `#9aa1ad` | `#5b626e` |
| `--text-placeholder` | `#7f8693` | `#6f7682` |
| `--text-disabled` | `#5f6570` | `#a2a8b2` |
| `--icon` | `#b8bec9` | `#3f4550` |
| `--icon-dim` | `#858c98` | `#6c7380` |

### The accent

The accent's tokens are mixed from `--accent-input`. With the default azure they come to: the fill
`#2f6fdf`, its hover `#4880e3`, the strong tone `#7ea6eb` dark and `#2659b2` light (strokes, text and
icons in the accent), and the subtle tone `#21365b` dark and `#ccd9f1` light (the selection behind
normal text).

| Token | Dark | Light |
|---|---|---|
| `--accent-fill` | `var(--accent-input)` | same |
| `--accent-fill-hover` | `color-mix(in srgb, var(--accent-input) 88%, white)` | same |
| `--accent-strong` | `color-mix(in srgb, var(--accent-input) 62%, white)` | `color-mix(in srgb, var(--accent-input) 80%, black)` |
| `--accent-subtle` | `color-mix(in srgb, var(--accent-input) 30%, #1b1e23)` | `color-mix(in srgb, var(--accent-input) 20%, #f3f4f6)` |
| `--on-accent` | `#ffffff` | same |

### Status

Each status has a colour for text and icons, a background and a border.

| Token | Dark | Light |
|---|---|---|
| `--danger` | `#f06b63` | `#c8372d` |
| `--danger-background` | `#3a1d1e` | `#fbe7e5` |
| `--danger-border` | `#6e2f2d` | `#f0b7b1` |
| `--warning` | `#e3b14f` | `#9a6a00` |
| `--warning-background` | `#3a2f17` | `#fbf1d9` |
| `--warning-border` | `#6b5321` | `#ead08c` |
| `--favourite` | `#f2c14e` | `#b07d00` |
| `--success` | `#4fbd85` | `#1e8a55` |
| `--success-background` | `#173326` | `#e1f4ea` |
| `--success-border` | `#25583d` | `#a6dcbf` |
| `--info` | `#63a4ff` | `#1f6fd6` |
| `--info-background` | `#172a44` | `#e3eefc` |
| `--info-border` | `#24497a` | `#a9c8f3` |
| `--shadow-color` | `rgba(0, 0, 0, 0.45)` | `rgba(20, 24, 32, 0.18)` |
| `--scrim` | `rgba(0, 0, 0, 0.4)` | `rgba(20, 24, 32, 0.25)` |

### The viewport

The axes as the gizmos draw them, and the HUD over any scene: the same in both schemes.

| Token | Dark | Light |
|---|---|---|
| `--axis-x` | `#e5534b` | same |
| `--axis-y` | `#6bc46d` | same |
| `--axis-z` | `#4a8cf0` | same |
| `--hud-background` | `rgba(18, 20, 24, 0.72)` | same |
| `--hud-text` | `#e8ebf0` | same |
| `--hud-text-dim` | `#a9b0bb` | same |

### Type categories

A type icon's tint in the lists the eye scans (the hierarchy, the assets, the component picker, the
inspector's headers). A type's category is asked of the type (a component's reflected
`category`); one with none keeps `--icon`.

| Token | Dark | Light |
|---|---|---|
| `--category-rendering` | `#6cb4ee` | `#1f6fb4` |
| `--category-physics` | `#ec8c5c` | `#b8501c` |
| `--category-audio` | `#e776ad` | `#b03a77` |
| `--category-scripting` | `#6fd093` | `#1d8048` |
| `--category-animation` | `#4ccbc0` | `#0d7a72` |
| `--category-gui` | `#c8a774` | `#85622a` |
| `--category-effects` | `#e8c35e` | `#8c6400` |
| `--category-navigation` | `#a395f2` | `#5f4bc9` |
| `--category-terrain` | `#a9c45f` | `#5a7417` |
| `--category-networking` | `#9fb0c8` | `#4f6078` |
| `--category-input` | `#d59be6` | `#8c3fa8` |

### Metrics, type and motion (both schemes)

| Token | Value |
|---|---|
| `--icon-size` | `16px` |
| `--radius-small` | `calc(var(--radius-input) - 2px)` |
| `--radius` | `var(--radius-input)` |
| `--radius-large` | `calc(var(--radius-input) + 3px)` |
| `--font-small` | `12px` |
| `--font-body` | `13px` |
| `--font-title` | `13px` (section and panel titles, semibold) |
| `--font-heading` | `16px` (page and dialog headings, semibold) |
| `--font-code` | `13px` |
| `--border-width` | `1px` |
| `--focus-width` | `2px` |
| `--motion-instant` | `0.05s` (feedback); `0s` with less motion |
| `--motion-fast` | `0.15s` (state, small popups); `0s` with less motion |
| `--motion-slow` | `0.3s` (dialogs); `0s` with less motion |
| `--easing` | `ease` |
| `--easing-out` | `ease-out-quint` |

### The density's sizes

Each density sheet sets these in whole pixels, so every edge lands on a pixel. Compact is the
editor's default.

| Token | Compact | Default | Comfortable | What it sizes |
|---|---|---|---|---|
| `--space-xs` | `2px` | `2px` | `2px` | spacing, smallest to largest |
| `--space-s` | `3px` | `4px` | `5px` |  |
| `--space-m` | `7px` | `8px` | `9px` |  |
| `--space-l` | `10px` | `12px` | `14px` |  |
| `--space-xl` | `14px` | `16px` | `18px` |  |
| `--row` | `22px` | `26px` | `30px` | list, tree, property and menu rows |
| `--control-height` | `22px` | `26px` | `30px` | fields, buttons, combos |
| `--bar` | `29px` | `34px` | `39px` | tool bars and panel headers |
| `--tab` | `22px` | `26px` | `30px` | tabs |
| `--menubar` | `20px` | `24px` | `28px` | the menu bar |
| `--statusbar` | `20px` | `24px` | `28px` | the status bar |
| `--glyph-box` | `14px` | `16px` | `18px` | a small glyph's square: a tab's close cross, the strip's list arrow |
| `--glyph-inset` | `3px` | `4px` | `5px` | the glyph inside it |
| `--strip-button` | `20px` | `24px` | `28px` | a tab strip's list and close-all buttons |
| `--arrow` | `14px` | `16px` | `18px` | a tree's or grid's arrow column |
| `--tree-indent` | `14px` | `16px` | `18px` | a tree level's indent |
| `--grid-indent` | `10px` | `12px` | `14px` | a property grid level's indent |

## Layer 3: the GUI's tokens

These are the tokens the GUI's widgets, docking, the toolkit and the code editor read. `tokens.css`
sets each from a semantic token (for example `--button-background: var(--element)`), so the widgets
follow the semantic layer with no rules of their own. Override one only to change that one widget:

`--accent`, `--selection`, `--focus-ring`, `--control`, `--control-raised`, `--control-hover`, `--chrome-raised`, `--chrome-hover`, `--surface-header`, `--track`, `--scrollbar`, `--thumb`, `--text-title`, `--outline`, `--grip`, `--overlay-hover`, `--row-hover`, `--disabled-opacity`, `--window-close-text`, `--button-background`, `--button-hover`, `--button-active`, `--button-text`, `--button-padding`, `--button-radius`, `--field-background`, `--field-text`, `--field-radius`, `--combobox-background`, `--combobox-text`, `--combobox-radius`, `--list-background`, `--list-selection`, `--list-hover`, `--list-text`, `--menu-background`, `--menu-highlight`, `--menu-separator`, `--menu-radius`, `--menubar-background`, `--menubar-hover`, `--menubar-selected`, `--menubar-selected-text`, `--tabbar-background`, `--tabpages-background`, `--tab-background`, `--tab-text`, `--tab-hover-background`, `--tab-checked-background`, `--tab-checked-text`, `--tab-dragging-background`, `--tab-dragging-text`, `--splitter-handle`, `--splitter-handle-active`, `--slider-track`, `--slider-fill`, `--slider-thumb`, `--progress-track`, `--progress-fill`, `--checkbox-box`, `--radio-ring`, `--radio-dot`, `--scrollview-background`, `--scrollbar-background`, `--scrollbar-thumb`, `--window-background`, `--window-title-background`, `--window-border`, `--window-radius`, `--dragpreview-background`, `--dragpreview-effect`, `--drop-refused`, `--table-header-background`, `--table-header-text`, `--toast-background`, `--toast-info`, `--toast-success`, `--toast-warning`, `--toast-error`, `--toast-width`, `--dock-background`, `--dock-stack-background`, `--dock-central-background`, `--dock-preview`, `--dock-preview-border`, `--toolbar-background`, `--toolbar-text`, `--toolbar-separator`, `--toolbar-button-radius`, `--toolbar-button-padding`, `--toolbar-button-hover`, `--toolbar-button-pressed`, `--toolbar-button-checked`, `--statusbar-background`, `--statusbar-text`, `--statusbar-message`, `--breadcrumb-text`, `--breadcrumb-current`, `--breadcrumb-hover`, `--breadcrumb-pressed`, `--breadcrumb-separator`, `--propertygrid-background`, `--propertygrid-name`, `--propertygrid-category`, `--propertygrid-component`, `--propertygrid-changed`, `--propertygrid-changed-name`, `--propertygrid-row-hover`, `--propertygrid-popup`, `--timeline-playhead`, `--timeline-marker`, `--timeline-marker-chosen`, `--curveview-background`, `--curveview-grid`, `--curveview-selected`, `--graphview-background`, `--graphview-grid`, `--graphview-grid-major`, `--graphnode-background`, `--graphnode-border`, `--graphnode-selected`, `--graphnode-radius`, `--dopesheet-background`, `--dopesheet-selected`, `--code-background`, `--code-text`, `--code-gutter`, `--code-line-number`, `--code-current-line-number`, `--code-current-line`, `--code-selection`, `--code-caret`, `--code-error`, `--code-warning`, `--code-info`.

## Contrast

WCAG contrast ratios with the default accent, computed from the values above (text needs 4.5:1,
large text and icons 3:1):

| Foreground | Background | Dark | Light |
|---|---|---|---|
| `--text` | `--surface` | 12.3:1 | 14.5:1 |
| `--text-strong` | `--surface` | 15.3:1 | 17.7:1 |
| `--text-dim` | `--surface` | 6.4:1 | 5.6:1 |
| `--text-placeholder` | `--surface-sunken` | 5.1:1 | 4.6:1 |
| `--text-disabled` | `--surface` | 2.9:1 | 2.2:1 |
| `--icon` | `--surface` | 9.0:1 | 8.8:1 |
| `--icon-dim` | `--surface` | 4.9:1 | 4.3:1 |
| `--text` | `--surface-raised` | 11.3:1 | 13.7:1 |
| `--text` | `--surface-overlay` | 10.3:1 | 16.0:1 |
| `--text-strong` | `--surface-sunken` | 17.0:1 | 19.5:1 |
| `--on-accent` | `--accent-fill` | 4.7:1 | 4.7:1 |
| `--text` | `--accent-subtle` | 8.8:1 | 11.2:1 |
| `--accent-strong` | `--surface` | 6.8:1 | 6.0:1 |
| `--danger` | `--surface` | 5.6:1 | 4.7:1 |
| `--warning` | `--surface` | 8.5:1 | 4.3:1 |
| `--success` | `--surface` | 7.1:1 | 4.0:1 |
| `--info` | `--surface` | 6.6:1 | 4.4:1 |
| `--danger` | `--danger-background` | 5.1:1 | 4.4:1 |
| `--warning` | `--warning-background` | 6.7:1 | 4.2:1 |
| `--success` | `--success-background` | 5.8:1 | 3.8:1 |
| `--info` | `--info-background` | 5.7:1 | 4.2:1 |

The type categories on `--surface`:

| Token | Dark | Light |
|---|---|---|
| `--category-rendering` | 7.5:1 | 4.8:1 |
| `--category-physics` | 6.7:1 | 4.5:1 |
| `--category-audio` | 6.0:1 | 5.1:1 |
| `--category-scripting` | 8.8:1 | 4.5:1 |
| `--category-animation` | 8.4:1 | 4.7:1 |
| `--category-gui` | 7.3:1 | 5.1:1 |
| `--category-effects` | 9.9:1 | 4.8:1 |
| `--category-navigation` | 6.5:1 | 5.7:1 |
| `--category-terrain` | 8.6:1 | 4.8:1 |
| `--category-networking` | 7.6:1 | 5.8:1 |
| `--category-input` | 7.7:1 | 5.6:1 |

Below 4.5:1: `--text-disabled` (meant to recede), `--icon-dim` in the light scheme, and, in the
light scheme, `--text-placeholder` on `--surface`, `--warning`, `--success` and `--info` on
`--surface`, and each status colour on its own background. The accessibility pass (the 0.1
backlog) takes these up.
