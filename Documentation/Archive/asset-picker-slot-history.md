# Asset picker slot  (archived)

> Status: ARCHIVED - fully built. Non-authoritative: this is the original build spec, kept
> as the record of what was built and why; present-tense truth is the code + tests.

> STATUS: BUILT 2026-08-16 (Fable, P1-P3 in one pass). The composite slot is
> [preview | name | Pick | Edit | Clear] per the user's Unity-style refinement
> (preview click LOCATES, the dedicated Pick button opens the picker, name-click
> also picks); the preview is the icon/thumbnail two-layer seam
> (SetPreviewIcon fallback + SetPreviewThumbnail wins - asset-thumbnails.md
> consumes it). Drop = accept-at-hover/validate-at-drop (the manager never
> calls OnDrop for a None effect), accent ring for match, error ring + toast +
> LOG_WARNING for mismatch; browser list rows + grid tiles are drag sources
> (AssetDragData bound per row, groups excluded). Reveal navigates to the
> owning group + selects + scrolls. Rulings + corrections below were applied
> as written. The inspector's
> resource-reference control, upgraded from a bare button to a 3-control slot with
> drag-drop. Cross-cutting: it is the widget EVERY `resource::Ref<T>` inspector
> field uses, so it lands on the reflected-inspector track
> (Documentation/Archive/reflected-inspector-history.md) and its "Edit" action is an
> activation path for the property-animation panel
> (Documentation/Archive/property-animation-editor-history.md, amendment A1). Not built yet.

## Goal

Replace the current `editor::app::AssetPickerSlot` (a plain `ui::Button` whose
`SetPreview` is reserved-but-undrawn) with a real reference control: an asset
preview + an Edit button + a Clear button, that is also a DROP TARGET for the asset
browser and can REVEAL its asset there. One widget, used by every resource-ref
field in the inspector (and anywhere else a typed asset reference is edited).

## Current state

- `AssetPickerSlot : ui::Button` (`Code/Editor/Editor.App/AssetPickerSlot.cppm`) -
  clickable to open a type-filtered `AssetPickerDialog` the consumer wires; a
  `SetPreview(SVGDrawable*)` that is stored but never rendered.
- Consumers: the inspector's resource-ref rows build one per `resource::Ref<T>`
  field (`InspectorViewImpl.cpp` `BuildResourceRefRow` / settings twin), open the
  `AssetPickerDialog` (type-filtered by asset-type NAME), and write the picked Guid
  through `SetResourceRefCommand<T>` (undoable). See reflected-inspector.md - that
  spec collapses the per-type dispatch; THIS spec upgrades the widget it produces.

## What the framework already gives us (build on, do not add)

- **Drag-drop:** pattern-A `View::AsDragSource()` / `View::AsDropTarget()`
  (`IDragSource` / `IDropTarget`) + `UIContext::DragDrop()` (`DragDropManager`).
  The asset browser (`AssetsView`) already drags items (ListView drag). So the slot
  becomes a drop target; the browser is already the drag source.
- **Toasts:** `foundation.ui.toolkit` `ToastHost` (used by the editor app) for the
  wrong-type warning.
- **Reveal:** `AssetsView` has `ScrollToPosition` for grid/list/tree; a
  reveal-by-Guid is a thin addition (find the instance's item -> select + scroll).
- **Picker + write path:** `AssetPickerDialog` (type-name filtered) +
  `SetResourceRefCommand<T>` (undoable Guid set + Rebind). Reuse both.

## The control

A horizontal composite (a small `View`, not a bare Button):

```
[ preview ][ asset name (grows, click = pick)      ][ Edit ][ Clear ]
   icon       "MyClip"  (or "(none)")                 pen     x
```

- **Preview** (left): the asset's TYPE ICON for now (thumbnails are a later track -
  the field is finally drawn, unlike today). Clicking the preview REVEALS the asset
  in the asset browser (select + scroll into view). No-op when empty.
- **Name / body** (center, grows): the referenced asset's name, or "(none)".
  Clicking the body opens the type-filtered `AssetPickerDialog` to (re)assign - the
  existing pick affordance, preserved.
- **Edit** button (icon): opens the asset FOR EDITING. Routing:
  - a `PropertyAnimationClipAsset` -> focus the in-scene animation PANEL with the
    clip loaded (per property-animation-editor.md A1: the panel is the editing
    surface; the standalone clip page is retired);
  - any other asset -> open its editor page (the same path a browser double-click
    takes).
  Disabled (dimmed, non-interactive) when the slot is empty.
- **Clear** button (icon): clears the reference (nil Guid) through the SAME undoable
  command the picker uses. Disabled when already empty.

## Behaviors

### Type-filtered drop target
The slot is configured with its accepted asset-type name(s) - the same list the
inspector already passes to the picker (e.g. `{"PropertyAnimationClipAsset"}`, or
`{"StaticMeshAsset","SkinnedMeshAsset"}`). As an `IDropTarget`:

- **Accept** a drag whose payload asset TYPE is in the accepted set: on drop, set
  the reference (the undoable command) and refresh. Highlight the slot while a
  compatible drag hovers (theme accent border).
- **Reject** a wrong-type drag: do NOT change the reference; show a WARNING (a
  `ToastHost` toast "X is not a <AssetType>" and a `LOG_WARNING`). Do not highlight
  (or show a reject cue) on hover of an incompatible payload.

The drag payload must carry the asset's Guid + its asset-type name. The asset
browser is the drag source; if its current payload lacks the type name, add it
(small addition to the browser's `AsDragSource`).

### Reveal in browser
Clicking the preview calls a new `AssetsView::Reveal(const Guid&)` (find the item
for that instance across grid/list/tree, select it, `ScrollToPosition`). The slot
reaches the active `AssetsView` through the editor context / a small reveal seam
(so the widget itself stays decoupled from the browser instance).

### Seam (how a consumer configures a slot)
The slot takes a small config, NOT hard-wired `T`:

```
struct AssetSlotConfig {
    Array<String> acceptedTypeNames;        // for the picker filter + drop match
    Guid          current;                  // current reference (nil = none)
    Function<void(const Guid&)> onAssign;   // undoable set (picker OR drop)
    Function<void()>            onClear;     // undoable clear
    Function<void()>            onEdit;      // open-for-editing (routing above)
    Function<void()>            onReveal;    // reveal in browser
};
```

This keeps the widget generic; the inspector's ref-picker (reflected-inspector.md)
fills the callbacks. `onAssign`/`onClear` route through `SetResourceRefCommand<T>`;
`onEdit`/`onReveal` route through the editor context. When the reflected-inspector
generic ref path lands, ONE call site builds every slot; until then each existing
`BuildResourceRefRow<T>` fills the config.

## Theming (per the design system)

All chrome from the theme (no hex in draw code, fallbacks only): background /
border / dim-text tokens like `CurveCanvas`; the compatible-drop highlight resolves
AccentColor; Edit/Clear are icon buttons from the editor icon set; disabled state
uses the dim token. Register a type name for the slot.

## Tests

- Drop accept/reject: a payload with an accepted type name calls `onAssign` once; a
  wrong-type payload calls neither `onAssign` nor `onClear` and raises the warning
  (assert the toast/log path, headless).
- Clear calls `onClear`; disabled when empty (Edit + Clear inert when Guid is nil).
- The config seam: assigning/clearing routes through the provided callbacks exactly
  once (undo granularity).
- Reveal calls `onReveal` with the current Guid.
- (Reflected-inspector integration test) every component resource-ref field builds a
  slot whose accepted-type set is non-empty - the same completeness tripwire that
  guards the ref-picker.

## Phasing

- P1 - the 3-control slot (preview icon + Edit + Clear + click-to-pick) + the config
  seam, wired into the existing `BuildResourceRefRow<T>` call sites. Edit routing +
  Clear (undoable). No drag-drop yet.
- P2 - drop target (type-filtered accept/reject + warning toast) + `AssetsView`
  drag payload carrying the type name.
- P3 - reveal-in-browser (`AssetsView::Reveal(Guid)`), preview click wired.
- (Later) real thumbnails replace the type icon.

Sequencing note: this can land alongside the property-animation editor (its Edit
routing needs the panel from that track's P1) and folds into the reflected-inspector
generic ref path when that lands (one slot builder instead of the per-type table).

## Open questions - Fable rulings (2026-08-16, binding)

1. **Reveal seam: an EditorContext hook.** `Function<void(const Guid&)>
   RevealAsset` on `editor::EditorContext` (the established OnNotice-style
   idiom); the app wires it to `AssetsView::Reveal(Guid)`, the slot's consumer
   calls through the context. No event plumbing; the slot stays decoupled.
2. **Edit routing: a generic EditorContext hook, minimal now.**
   `Function<void(const Guid&)> OpenAsset` on the context; the app implements
   it as Guid -> `IContentDatabase::GetInstance(id)` -> the SAME
   `OpenInstancePage` path the browser double-click takes. When the
   property-animation panel lands (its spec A1), the app's implementation
   grows a type branch for clips - one place, no per-call-site wiring.
3. **Drag payload: a dedicated typed struct.** `AssetDragData : ui::DragData`
   (format `"asset/instance"`; Guid + asset-type name + display name) in
   editor.app. There is NO existing browser drag payload to extend - see
   correction C2.

## Fable corrections (what the draft got wrong about the substrate)

- **C1 - the inspector's ref rows do NOT use AssetPickerSlot today.** They use
  `ResourceRefEditor` (Editor.Scene, a PropertyEditor whose editor view is a
  bare `ui::Button` with OnPick). `AssetPickerSlot` is only used by the
  material LIST-slot rows. The upgrade therefore lands in BOTH places: the
  new composite AssetPickerSlot replaces `ResourceRefEditor`'s bare button
  (Edit/Clear/Reveal appear on every Ref<T> row) AND the list-slot rows keep
  using the same widget (pick-only affordance there). The entity-ref twin
  keeps using the widget with only OnPick wired - which drives a design rule:
  **buttons render only when their callback is wired**, so non-asset
  consumers degrade to a plain name button automatically.
- **C2 - the asset browser does not drag anything today.** No DragData exists
  anywhere in Editor.App; "the browser already drags items" was wrong. P2
  ADDS the drag source (grid tiles + list rows begin a drag with
  AssetDragData via the pattern-A View::AsDragSource seam) - it does not
  extend one.
- **C3 - config shape.** The AssetSlotConfig struct collapses into plain
  Function members + setters on the widget (OnPick/OnEdit/OnClear/OnReveal/
  OnAssignDropped + SetAcceptedTypes + SetValue) - the row builders wire them
  directly; a config struct adds a layer nothing needs. The widget raises
  LOG_WARNING itself on a rejected drop and exposes OnRejectedDrop for the
  consumer's toast (Context::Notify).
