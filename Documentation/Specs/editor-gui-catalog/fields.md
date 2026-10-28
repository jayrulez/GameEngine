# Value editing vocabulary: catalog of property editors, fields and inline controls

Scope: every value editor the editor uses today (old `foundation.ui.toolkit`, `Code/Foundation/UI.Toolkit`),
the editor-registered editors (`Code/Editor/Editor.App`, `Editor.Scene`, page-local subclasses), the
reflection hints that pick them, the hand-built fields outside any grid, and the equivalents in the new GUI
(`Code/Foundation/GUI.Toolkit` property grid, `Code/Foundation/GUI/Widgets`).

Key facts up front:

- The editor uses ONLY the old toolkit grid (`ui::toolkit::PropertyGrid`). Nothing under `Code/Editor` uses
  `gui::PropertyGrid`; the new grid is exercised only by `Samples/GUISandbox`.
- Old grid: 11 toolkit editor classes + 6 editor-defined `PropertyEditor` subclasses + 3 slot/field widgets
  used as values (AssetPickerSlot, CompactAssetSlot, ShortcutCaptureButton) + tool-panel helpers.
- 21 places build a `ui::toolkit::PropertyGrid` (17 editor pages/panels, 2 inspector tabs, plus terrain/vegetation
  tool panels; list in section 6).
- About 158 direct `MakeRef<...Editor>` construction sites in `Code/Editor` (not counting tests), multiplied by
  page-local row helpers (Particles has about 108 `Row*` calls, AnimationGraph about 25, tool panels 23).
- About 80 hand-built value fields outside any grid (section 7), plus a 5th and 6th reflection-to-widget mapping
  (SettingsDialog and GenericAssetForm) that do not go through the inspector's mapping.

Legend for "States": MV = mixed value (multi-object), RO = read-only shown, DIS = disabled, CHG = changed-from-default
marker, REV = revert, VAL = validation error shown, OOR = out-of-range feedback, ESC = Escape cancels.

---

## 1. Grid frames

### 1.1 Old: `ui::toolkit::PropertyGrid`
File: `Code/Foundation/UI.Toolkit/PropertyGrid.cppm`, base `Code/Foundation/UI.Toolkit/PropertyEditor.cppm`.

- Look: ScrollView of FlexLayout rows; uncategorized rows first, then one `Expander` per category (first-seen
  order), nested categories via `SetCategoryParent`, header icon actions via `SetCategoryHeaderActions`,
  `SetCategoryDefaultCollapsed`; expansion state remembered across rebuilds by header text.
- Row: `Label` (12px, ellipsized) with FlexGrow = `LabelWidthRatio` (0.4 fixed) + editor view with FlexGrow 0.6,
  6px gap; row spacing 6px. `RowHeight = 26` is declared but never read (rows size to content).
- Optional editable label (`OnLabelRenamed` makes it an `EditableLabel`): no caller in `Code/Editor` uses it.
- Row tooltip from `SetTooltip`, live display name via `SetDisplayName`, conditional visibility `SetRowVisible`.
- Gesture API on the editor: `OnEditBegin / OnValueChanged / OnEditEnd / OnEditCancelled`. NO consumer in
  `Code/Editor` subscribes to these (grep: only CurveCanvas/GradientEditor own events are used). Undo is done by
  the setter callbacks plus command merging (see section 5).
- Missing at grid level: mixed values (inspector edits only the primary selection), changed-from-default marker,
  revert to default, copy/paste of a value, search/filter, draggable label split, keyboard row navigation,
  virtualization (the generic asset form had to default-collapse groups because ~1300 rows made it crawl).

### 1.2 New: `gui::PropertyGrid`
Files: `Code/Foundation/GUI.Toolkit/PropertyGrid.cppm`, `PropertyEditor.cppm`, `PropertyEditors.cppm`,
`CurveEditors.cppm` (default registration), `PropertySource.cppm`, `ReflectedSource.cppm`.

- Rows pooled on an item view (only visible rows exist), one pool per editor type.
- Editor chosen by a registry: hint first (`RegisterHint`), then kind (`RegisterKind`), else
  `UnknownPropertyEditor` ("No editor for <hint or kind>").
- Grid features: search filter (name or path), draggable label split (`label-ratio` 0.1..0.9), indent per depth,
  changed marker (3px bar, `::marker` colour) when any object differs from its default, row menu with
  Copy / Paste / Revert (text from the source), list element grip drag + Insert above / Remove / Move up / Move down,
  wheel always scrolls the grid (never changes values), an edit's row is not rebound while it previews.
- Gesture host: `BeginPropertyEdit / PreviewPropertyEdit / CommitPropertyEdit / CancelPropertyEdit`, lists via
  `EditPropertyList` (one undo entry each).
- `PropertyInfo` carries: Name, DisplayName, Category, Description, Kind, Hint, HasRange/Min/Max/Step, Choices,
  ReadOnly, Visible, ShowsAlpha, ListOps, MaxCount, ElementTypes. No unit, no validation message, no
  "decimal places", no per-row actions.

---

## 2. Reflection hints and what they pick

Attribute keys found via `.PropAttribute(...)` / `.Attribute(...)` across the codebase (counts are declaration sites).

| Hint (where declared) | Count | Old grid (scene inspector / settings rows) | Other old consumers | New grid (`ReflectedSource`) |
|---|---|---|---|---|
| `range` Float4 {min,max,step,_} (prop) | 75 | f32 only: `RangeEditor` (slider + field). Ignored on ints (IntEditor gets full i64), ignored on Float2/3/4 | `SettingsDialog` u32 rows: NumericField min/max; asset pages re-type the numbers by hand (Texture anisotropy 1..16, Font 8..128) | any number: `HasRange`; a float also hint `slider` -> `SliderPropertyEditor` (slider + field, X6); an integer a bounded `NumberPropertyEditor` |
| `displayName` (prop) | 127 | row label (else `PrettifyPropertyName`) | ContainerList label | DisplayName (else `ReadablePropertyName`) |
| `displayName` (type) | 89 | component/section header, Add Component menu | settings category name | list element type names |
| `description` (prop) | 121 | row tooltip | ContainerList tooltip | row tooltip |
| `description` (type) | 38 | Add Component menu tooltip | | |
| `category` (type) | 62 | Add Component menu grouping ("Other" fallback) | | |
| `visibleWhen` String "prop" / "prop=1,2" | 64 | `SetRowVisible` via refresher | | `Visible` (all objects must match) |
| `boneName` (prop, String) | 8 | `EnumEditor` of the animator's skeleton bones + "(none)" + "(not in the skeleton)"; falls back to StringEditor | | none (plain string) |
| `sceneOnly` (prop) | 4 | excluded from settings-profile rows | | none |
| `flags` (prop, enum) | 2 | ignored: enum shows a ComboBox, combined bits unrepresentable | | `FlagsPropertyEditor` |
| `noAlpha` (prop, Color) | 2 | ignored: picker always shows alpha | | `ShowsAlpha=false` |
| `hdrColor` (prop, Float4) | 2 | ignored: shows `Float4Editor` (4 numbers) | | hint `ColorHdr` -> `HdrColorPropertyEditor` |
| `PropertyFlags::ReadOnly` | n/a | setter passed as null, widget STILL editable | | `ReadOnly` -> controls disabled / read-only |
| `css`, `access`, `accessRole`, `accessActivate`, `twoWay`, `translatable`, `scriptName`, `element`, `maxLegs` | 156/23/36/4/10/8/7/3/1 | not editor hints (GUI styling, accessibility, binding, scripting) | `GUI.Inspector/Queries.cppm` reads `css` | not editor hints |

Implicit "hints" by type in the new grid (`ReflectedSource::DescribeType`): Float2 / Float3 / Float4 shapes get hint
`Float2|Float3|Float4` (FloatsPropertyEditor covering children); Quaternion gets hint `Float3` with children
pitch/yaw/roll (step 1); nested structs and unknown types get their TYPE NAME as hint (so an application can
register a per-type editor); registered curve/gradient types get kinds Curve/Gradient; `stops` hint is set on an
HDR colour's intensity part; `slider` on a float with a `range`. Registered hints by default: `Float2`, `Float3`, `Float4`,
`slider`, `stops`, `ColorHdr`. (Until X6 no slider editor was registered.)

Non-reflection "hint" channels in the old editor:
- Script properties (`ScriptPropertyType`): Float, Int, Bool, String, Color, Vec3, Entity, Asset (`assetType` +
  "Asset" suffix, empty = any asset). `InspectorViewImpl.cpp:2201-2350`.
- Shader properties for materials: `hasRange / rangeMin / rangeMax`, plus NAME heuristics: Metallic, Roughness,
  OcclusionStrength, AlphaCutoff -> 0..1 slider; NormalScale -> 0..2. `MaterialPageImpl.cpp:600-625`.
- Generic asset form scalar kinds: Bool, Float32/64, ints, Text, Guid, Blob, ArrayCount. `AssetFormPageImpl.cpp:467-593`.

---

## 3. Old toolkit editor kinds (`Code/Foundation/UI.Toolkit`)

Common to all: one row, label left at 40%, value right; no MV, no CHG, no REV, no VAL; RO only by the caller
passing a null setter (the control stays live and the refresher snaps it back next frame); OOR silently clamps.
Call-site counts are direct `MakeRef<X>` sites outside tests, by library.

### 3.1 BoolEditor
- File: `BoolEditor.cppm`. Value: bool. Selected by: `bool` reflected type, script Bool, generic Bool.
- Look: bare `CheckBox` (no text) in the value column.
- Undo: Begin + set + End on each toggle (one click = one gesture).
- States: none beyond checked/unchecked; old CheckBox has no indeterminate state.
- Used: Animation 6, App 1 (ToolPanelWidgets), Audio 1, Generic 1, Particles 1 (helper, 12 calls), Scene 5,
  Terrain 2, Texture 1 = 18 sites.
- New GUI: `BoolPropertyEditor` (CheckBox, indeterminate when mixed, disabled when RO).
- Problems: no RO look; the click target is only the box (label column is not clickable); dialogs elsewhere use
  CheckBox WITH its text on the right, so a bool reads two different ways.

### 3.2 IntEditor
- File: `IntEditor.cppm`. Value: i64 (NumericField f64 underneath, 0 decimals, step 1).
- Selected by: every integer width i8..u64 (scene inspector), script Int, generic ints, page code.
- Look: `NumericField` with spin buttons; drag inside the text SELECTS text (no drag-to-scrub); Up/Down step,
  PageUp/PageDown 10 steps, wheel steps only while focused, select-all on focus.
- Undo: BeginEdit on focus, EndEdit on blur, Escape restores pre-edit value and calls the setter (ESC).
- Used: Animation 2, Fonts 4, Generic 1, Heightfield 1, Materials 1, Particles 2 (helper, 10 calls), Scene 2 = 13 sites.
- New GUI: `NumberPropertyEditor` (integer mode).
- Problems: the inspector ignores `range` and the integer width: min/max are full i64, so typing 300 into a u8
  (`RigidBodyComponent.collisionGroup`) wraps via `static_cast` with no feedback; i64/u64 beyond 2^53 lose precision
  through the f64 field; invalid text silently reverts; no scrub; spinner clicks fire the setter outside a focus gesture.

### 3.3 FloatEditor
- File: `FloatEditor.cppm`. Value: f64 (written back as f32 or f64). Ctor: min, max, step, decimal places.
- Selected by: `f32` without `range`, script Float, generic Float32/64, many page rows.
- Look and keys: as IntEditor (spinners, no scrub, wheel only when focused).
- Undo: focus gesture; ESC restores.
- Defaults differ per caller: inspector -1e9..1e9 step 0.1, 2 decimals; script 3 decimals; generic form
  -1e12..1e12 step 0.01, 4 decimals; behaviour Update Interval 0..3600 step 0.05, 3 decimals; particles helper step 0.05.
- Used: Animation 3, App 1 (AddToolPanelFloat, 23 calls via terrain/vegetation panels), Audio 1, Fonts 2,
  Generic 1, Heightfield 2, Materials 2, Particles 1 (helper, 39 RowFloat calls + range/shape rows),
  Scene 4, Terrain 3 = 20 sites.
- New GUI: `NumberPropertyEditor` (float mode, step 0.01 default, scrub, expressions, mixed).
- Problems: 2 decimals hides small values (0.004 shows 0.00) while the stored value differs; no unit
  (units are baked into labels on some pages: "MSDF Size (px)", "Field Range (texels)"); angles in radians
  shown raw (particle cone angle 0..3.1416); no range shown; no scrub.

### 3.4 RangeEditor
- File: `RangeEditor.cppm`. Value: f32. Selected by: f32 with `range` attribute (inspector, settings rows);
  material 0..1 heuristics and shader ranges; texture anisotropy.
- Look: `Slider` (grows) + NumericField fixed width computed from |min|/|max| (60px minimum), 2 decimals.
- Undo: slider drag start/end = gesture; field focus = gesture; no Escape cancel on either part.
- Used: Materials 1, Scene 2, Texture 1 = 4 sites (but drives every ranged reflected f32: 75 range declarations).
- New GUI (X6): `SliderPropertyEditor` (hint `slider`): the slider beside its field, one gesture either way, Escape
  putting a drag or a scrub back, the wheel left to the grid; set by `ReflectedSource` on a ranged float and by the
  material page on its ranged floats.
- Problems: step from the attribute applies to the slider but the field uses 0.1 when step is 0; 2 decimals
  regardless of step (a 0.001 step shows 2 digits); radians ranges (0..1.55, 0..6.2832) shown raw;
  min/max not labelled; two controls for one value.

### 3.5 StringEditor
- File: `StringEditor.cppm`. Value: String. Selected by: `String` (inspector), script String, generic Text,
  Guid text twin ("<label> id"), Blob placeholder, many read-only info rows.
- Look: single-line `EditText`. No multiline, no placeholder, no max length.
- Undo: Begin on focus; commit on Return (OnSubmit) and on blur; Escape restores (ESC).
- Used: Animation 6, Audio 1, Fonts 3, Generic 3, Heightfield 1, Image 1, Particles 2, Scene 3, Shaders 1,
  Texture 3 = 24 sites.
- New GUI: `StringPropertyEditor` (TextField, Mixed placeholder, read-only flag). GUI has `TextArea` for multiline
  but no editor uses it.
- Problems: (a) "read-only" rows are StringEditors with a null setter (Texture "Warning", "Source", "Cooks to"
  `TextureEditorPageImpl.cpp:352,402,411`; Font "File" `FontEditorPageImpl.cpp:564`; Image/Heightfield "Source"
  `ImageEditorPageImpl.cpp:213`, `HeightfieldEditorPageImpl.cpp:216`; Particles "Systems" `ParticleEffectPageImpl.cpp:1416`;
  generic Blob `AssetFormPageImpl.cpp:481`): the user can type, nothing happens, no RO look. (b) After Return the
  gesture has ended but focus stays: the inspector's per-frame refresher (`!IsEditing()`) rewrites the text, so
  further typing in the same focus session is overwritten and the blur commit is skipped (likely bug,
  `StringEditor.cppm` OnSubmit -> EndEdit, `InspectorViewImpl.cpp:2679`). (c) Guid typed as text: invalid input is
  dropped silently (`AssetFormPageImpl.cpp:535`).

### 3.6 EnumEditor
- File: `EnumEditor.cppm`. Value: i32 index into a string list. Selected by: `IsEnum` (inspector, settings rows),
  `boneName` hint (bone list), page enums.
- Look: `ComboBox` (popup list; Space/Return open, Up/Down step, Escape closes).
- Undo: Begin + set + End per selection.
- Used: Animation 1 (helper, 6 calls), Audio 1, Fonts 1, Image 1, Materials 3, Particles 1 (helper, 7 calls),
  Scene 3, Texture 6 = 17 sites.
- New GUI: `EnumPropertyEditor` (ComboBox with Mixed placeholder, type-ahead letter select, readable names).
- Problems: items are RAW enumerator names in the inspector (`v.name`), readable in the new grid; a value not in
  the list shows index 0 silently (`indexOf` returns 0); flag enums impossible; no description per item.

### 3.7 ColorEditor
- File: `ColorEditor.cppm`, picker `ColorPicker.cppm`. Value: Color (LDR). Selected by: `Color` type,
  script Color, material colour uniforms, particle colours.
- Look: `ColorView` swatch filling the value column; click opens a MODAL `Dialog` "Color Picker" with
  SV square, hue strip, alpha strip, RGB fields, hex input, current/original swatches, OK/Cancel.
- Undo: Begin on open, live setter during drag, OK = End, Cancel = restore + CancelEdit (ESC through Cancel).
- Used: Materials 1, Particles 1 (helper, 4 calls + 2 range colours), Scene 3 = 5 sites.
- New GUI: `ColorPropertyEditor` (swatch + hex field on the row, non-modal popup picker, `noAlpha`, mixed hatch).
- Problems: no hex/value on the row (must open a dialog to read a value); modal dialog blocks the viewport while
  picking; no keyboard path to open (mouse only, `OnMouseDown`); alpha always shown; HDR impossible.

### 3.8 Float2Editor / Float3Editor / Float4Editor
- Files: `Float2Editor.cppm`, `Float3Editor.cppm`, `Float4Editor.cppm` (axis labels from `VectorFields.cppm`).
- Value: Float2/3/4. Selected by: those types (inspector), Transform position/rotation/scale, script Vec3,
  material vec uniforms, particle vectors, Float4 also for `hdrColor` Float4 (hint ignored).
- Look: one NumericField per axis, coloured axis letter prefix (X red, Y green, Z blue, W gold), no spinners,
  3 decimals, 4px gap.
- Undo: Begin on first field focus, End on blur of that field; a Tab to the next axis ends one gesture and
  starts another; NO Escape cancel.
- Used: Float2: Animation 1, Heightfield 1, Materials 1, Particles 1 (helper, 3 calls + range pairs), Scene 1 = 5;
  Float3: Materials 1, Particles 1 (helper, 14 calls), Scene 6 = 8; Float4: Materials 1, Scene 1 = 2.
- New GUI: `FloatsPropertyEditor` (hint Float2/3/4): each part a full `NumberPropertyEditor` with its own path,
  scrub, mixed per axis. Axis colours are left to the sheet (`label.component`).
- Problems: no scrub on the axis letter (the obvious DCC affordance); no "link/uniform scale" toggle; ranges
  hardcoded -100000..100000 everywhere; no Escape; narrow panels squeeze 4 fields with no wrap.

### 3.9 ButtonEditor
- File: `ButtonEditor.cppm`. Value: none (action). `SetButtonEnabled`.
- Look: a `Button` whose TEXT is the row name, next to a label with the same name.
- Used: Animation 1 (helper, 2 calls), Fonts 1, Scene 6 (Bake Navigation, Revert to Prefab, profile
  Open/Make/Copy, a watcher row) = 8 sites.
- New GUI: none.
- Problems: label and button repeat the same words ("Bake Navigation | [Bake Navigation]"); "Revert to Prefab *"
  marks overrides by renaming the LABEL while the button text stays (`InspectorViewImpl.cpp:1209-1232`); a
  component-level revert stands in for per-field override markers.

### 3.10 Standalone fields not used by the editor
- `VectorFields.cppm`: `Vector2Field`, `Vector3Field`, `Vector4Field`, `QuaternionField` (Euler X/Y/Z degrees,
  aggregated gesture across sibling fields via the mutation queue). Zero uses outside tests.
- `HDRColorPicker.cppm`: LDR picker + intensity multiplier to a Float4. Zero uses.
- `CurveCanvas.cppm`, `GradientEditor.cppm`: used only through the particle editors and the property-animation clip view.

---

## 4. Editor-defined editors and slots (`Code/Editor`)

### 4.1 ResourceRefEditor + AssetPickerSlot (asset reference)
- Files: `Code/Editor/Editor.App/ResourceRefEditor.cppm`, `ResourceRefEditorImpl.cpp`, `AssetPickerSlot.cppm`,
  picker `AssetPickerDialog.cppm`.
- Value: Guid of an asset; accepted types are a ctor argument (`AnyAssetType()` "*", `EntityType()` "@entity").
- Selected by: 17 hardcoded `Ref<T>` types in `SceneInspectorView::BuildPropertyRow` (`InspectorViewImpl.cpp:1291-1406`,
  each with a literal asset-type list), any `reference`-shaped field in `SettingsRows::BuildReferenceRow`
  (types from `SourceAssetTypesOf`), script `Asset` properties, generic form Guids (any asset), page code.
- Look: `[ name button (grows, click = pick) ][ preview/reveal (type glyph or thumbnail) ][ Edit ][ Clear ]`;
  trailing buttons only when wired; Edit/Clear/reveal disabled when empty; drop target with accent ring (match)
  or error ring (mismatch) and a toast naming the accepted types.
- Undo: pick, drop and clear go through ONE assign callback (one undo step each, not merged).
- States: empty text configurable ("(none)", "(built-in)", "Default"); "(missing)" for a dangling entity id.
- Used: Animation 2, App 2, Audio 1, Generic 1, Geometry 1, Materials 3, Particles 2, Physics 1, Render 1,
  Scene 6, Terrain 1 = 21 sites.
- New GUI: none. A `Ref<T>` resolves to `Shape::Other` -> type-name hint -> `UnknownPropertyEditor`.
- Problems: the asset name is a plain button (no search-as-you-type, no recent list, no keyboard pick); modal
  picker; empty-state wording differs per page; inspector type lists duplicated per Ref type while settings rows
  derive them; thumbnail is a 14px glyph.

### 4.2 Entity reference (ResourceRefEditor::BindEntity)
- Files: as 4.1 plus `Code/Editor/Editor.Scene/EntityPickerDialog.cppm`.
- Value: Guid of a scene entity. Selected by: `scene::EntityRef` (`InspectorViewImpl.cpp:2490`), script Entity
  (`:2360`).
- Look: same slot; pick opens a modal filterable entity tree; a hierarchy row can be dropped.
- Undo: one assign per pick/drop/clear (script Entity clear removes the override).
- New GUI: none.
- Problems: no "pick from viewport" eyedropper; "(none)" vs "(missing)" text only; no jump-to-entity verb.

### 4.3 CompactAssetSlot
- Files: `Code/Editor/Editor.App/CompactAssetSlot.cppm`, `CompactAssetSlotImpl.cpp`.
- A caption + the same slot at toolbar height for preview bars. Used: Animation 4.
- New GUI: none.

### 4.4 ContainerListEditor (lists)
- Files: `Code/Editor/Editor.App/ContainerListEditor.cppm`, `ContainerListEditorImpl.cpp`.
- Value: a list. Two shapes: SLOT rows (AssetPickerSlot + move up/down/remove icons) or SECTION elements
  (`ElementsAsSections`, each element a nested grid section with icon actions in its header).
- Selected by: reflected containers (`BuildContainerRows`, `InspectorViewImpl.cpp:2721`), mesh materials,
  script behaviours, terrain paint layers, animation lists, project font list.
- Undo: each add/remove/move/assign one command (MutateComponent).
- Used: Animation 3, App 1, Particles 1, Scene 3, Terrain 1 = 9 sites.
- New GUI: `ListPropertyEditor` (count + Add, type menu for polymorphic) with element rows, grip drag and row menu.
  Missing there: drop-to-append of assets, slot rows.
- Problems: struct-element leaf editing is "a follow-up" (comment `InspectorView.cppm:500`); element pick is
  specialised to `Ref<Material>`; three spellings of empty ("None", "(none)", "(runtime)").

### 4.5 CurveFieldEditor (particles)
- File: `Code/Editor/Editor.Particles/ParticleEffectPageImpl.cpp:268`.
- Value: `ParticleCurveFloat` (Hermite keys). Look: an inline `CurveCanvas`, fixed 120dp tall, in the value column.
- Undo: every key change/add/remove and edit end writes back and commits a keyed edit.
- Used: 1 class, 6 `RowCurveFloat` calls.
- New GUI: `CurvePropertyEditor` (row preview, popup with ruler and curve view, commit once on close, Escape cancels).
- Problems: `RefreshView` is empty, so undo/redo does not update the canvas until a rebuild; linear keys are
  converted to cubic Broken on write-back (lossy); 120dp of a 40% narrower column.

### 4.6 GradientFieldEditor (particles)
- File: `ParticleEffectPageImpl.cpp:367`. Value: `ParticleCurveColor`. Look: inline `GradientEditor` 40dp tall.
- Interaction: click strip adds a stop, drag moves, right-click deletes, double-click fires `OnStopColorRequested`.
- Problems: NOTHING subscribes to `OnStopColorRequested` anywhere (grep), so a stop's colour cannot be edited;
  colour and alpha stops are merged/resampled on load; empty `RefreshView`.
- New GUI: `GradientPropertyEditor` (preview + popup gradient view).

### 4.7 NoticeEditor
- File: `Code/Editor/Editor.Scene/InspectorView.cppm:195`. A wrapped advisory label in the value column
  (e.g. RigidBody Cooked with no collision shape). Used: Scene 1.
- New GUI: none. Problems: no severity icon/colour; texture page shows warnings as an editable StringEditor instead.

### 4.8 CollisionMatrixEditor
- File: `InspectorView.cppm:157`, body `InspectorViewImpl.cpp:150-272`. Value: named collision groups + symmetric
  u32 masks. Look: header of rotated column names, per row an `EditText` name (fixed width) and a `CheckBox` per
  column, remove icon only on the last row, add button. Undo: whole-block command per rename/toggle.
- Used: Scene 1. New GUI: none (a layer-mask/matrix editor kind is missing everywhere).
- Problems: the only "layers/masks" editor; a mask field elsewhere is a plain IntEditor; no names on hover per cell.

### 4.9 Bone name picker
- `InspectorViewImpl.cpp:2416`. `boneName` String shown as `EnumEditor` of bone names. Falls back to a text field
  when no animator/skeleton. New GUI: none (would need dynamic Choices).

### 4.10 ShortcutCaptureButton (key binding)
- File: `Code/Editor/Editor.App/ShortcutCapture.cppm`. A button showing a chord; click arms capture, next key
  becomes the chord; Escape cancels, Backspace/Delete clears, modifier-only ignored, blur cancels.
- Used: Preferences > Shortcuts (`PreferencesDialog.cppm:390`). New GUI: none.

### 4.11 Input map keycap (key binding, second implementation)
- `Code/Editor/Editor.Input/InputMapPageImpl.cpp` `AddKeycap` (~:1217): a styled Button "Press..." while listening,
  page-level listen state, click again cancels. 6 call sites. New GUI: none.

### 4.12 Tool-panel kit
- `Code/Editor/Editor.App/ToolPanelWidgets.cppm`: `SegmentedToggle` (exclusive ToggleButtons, enum as buttons),
  `AddToolPanelFloat` (FloatEditor), `AddToolPanelBool` (BoolEditor). Used by terrain sculpt/splat and vegetation
  paint panels (`Editor.Terrain/ToolPanelsImpl.cpp:197,244,321`; 23 Add* calls in Terrain/Vegetation) and
  `Editor.App/ViewModeTogglesImpl.cpp:21`.
- New GUI: no segmented control (closest: `RadioButton` + ButtonGroup, `TabBar`).

---

## 5. Undo and commit behaviour, by host

| Host | Mechanism | Gesture boundary |
|---|---|---|
| Scene inspector components | setter -> `SceneEditContext::SetComponentProperty(Raw)` -> command merged with stack top per (entity, component, path, property) | none: two separate drags or two typed values on the same field in a row become ONE undo entry |
| Entity name | `RenameEntity` merged per entity | same |
| Asset refs / entity refs | one command per assign, not merged | per pick |
| Settings rows (scene tab, profile page) | `SettingsAccess::set` commands | merge per property |
| Script overrides | `MutateComponent` / settings blob per change | merge by type |
| Asset pages (Particles, AnimGraph, Texture, Material, Font, Generic, Audio, Input) | setter mutates in place + `CommitEdit(key)` / `ApplyEdit(key)` keyed coalescing | per key, same no-boundary issue |
| Dialogs (Preferences, Export, Project Settings) | staged, applied on Save | Save/Cancel; EXCEPT Preferences domain-contributed bools apply immediately (`PreferencesDialog.cppm:216`) |
| New GUI grid | Begin / Preview / Commit / Cancel through the source; Commit puts originals back then applies once | explicit gesture, Escape cancels a scrub/picker |

`PropertyEditor::OnEditBegin/OnEditEnd/OnEditCancelled` in the old toolkit are fired but never consumed.
Escape-cancel exists only in Float, Int, String (field) and Color (Cancel button); not in Float2/3/4, Range,
Enum, Bool, slots.

---

## 6. Pages and panels that build a property grid (old toolkit), with editors used

| File:line (grid) | Editors used |
|---|---|
| `Editor.Scene/InspectorView.cppm:385,419` (+ `InspectorViewImpl.cpp`) | all toolkit kinds, ResourceRef, EntityRef, ContainerList, Notice, CollisionMatrix, Button, bone picker, script rows |
| `Editor.Render/SettingsProfilePageImpl.cpp:114` | SettingsRows (Enum, Range, Float, Color, Bool, Float3, ResourceRef) |
| `Editor.Generic/AssetFormPageImpl.cpp:423` | String, Bool, Float, Int, ResourceRef + Guid text twin, Blob as String |
| `Editor.Materials/MaterialPageImpl.cpp:118` | ResourceRef, Enum, Range, Float, Float2/3/4, Int, Color (+ separate intensity Float for HDR) |
| `Editor.Texture/TextureEditorPageImpl.cpp:151` | Enum x6, String (read-only info x3), Bool, Range |
| `Editor.Fonts/FontEditorPageImpl.cpp:149` | String, Enum, Float (bounded), Int x4, Button, read-only String |
| `Editor.Particles/ParticleEffectPageImpl.cpp:718` | Row helpers: Float 39, Float3 14, Bool 12, Int 10, Enum 7, Curve 6, Color 4, Float2 3, ranges as min/max PAIRS of rows, Gradient 2, ResourceRef, ContainerList |
| `Editor.Animation/AnimationGraphPageImpl.cpp:479` | Row helpers Float/Int/Bool/Enum/Button/String, ResourceRef, ContainerList, Float2 |
| `Editor.Animation/AnimationClipPageImpl.cpp:199` | String, Bool, ContainerList, Float |
| `Editor.Animation/SkeletonPageImpl.cpp:176` | String |
| `Editor.Audio/BusLayoutPageImpl.cpp:313` | Float, Bool, String, Enum |
| `Editor.Heightfield/HeightfieldEditorPageImpl.cpp:71` | Int, Float2, Float x2, read-only String |
| `Editor.Image/ImageEditorPageImpl.cpp:95` | Enum, read-only String |
| `Editor.Shaders/ShaderPageImpl.cpp:142` | String |
| `Editor.Terrain/TerrainEditorPageImpl.cpp:288,360` | ContainerList (sections), ResourceRef maps, Float, Bool |
| `Editor.Terrain/ToolPanelsImpl.cpp:213,263,380` | tool-panel Float/Bool + SegmentedToggle |
| `Editor.Vegetation/VegetationToolPanelsImpl.cpp:175,299` | tool-panel Float/Bool |

Asset pages WITHOUT a grid that edit values by hand: SoundCue, AudioClip, InputMap, PropertyAnimation clip view,
GamePage (section 7).

---

## 7. Hand-built fields outside the property grid (file:line, kind, replacement)

Paths relative to `Code/Editor/`.

### 7.1 Dialogs
| Location | Field | Value | Could be |
|---|---|---|---|
| `Editor.App/PreferencesDialog.cppm:61,63` | EditText "UI font (.ttf)", "Mono font (.ttf)" | file path, no Browse | path field (missing kind) |
| `Editor.App/PreferencesDialog.cppm:72` | Slider + fixed "1.00x" label | f32 UI scale 0.5..2 | Range editor with suffix |
| `Editor.App/PreferencesDialog.cppm:102,137` | CheckBox with text | bool | Bool |
| `Editor.App/PreferencesDialog.cppm:127` | EditText "Templates root" | folder path | path field |
| `Editor.App/PreferencesDialog.cppm:152` | EditText "MCP port" | u32 typed as TEXT | Int with range |
| `Editor.App/PreferencesDialog.cppm:153` | EditText "MCP token" | secret, shown in clear | String (password) |
| `Editor.App/PreferencesDialog.cppm:216` | CheckBox per domain contribution | bool, applies immediately | Bool |
| `Editor.App/PreferencesDialog.cppm:291,299` | EditText name + two NumericField (1..16384) "x" | resolution preset list | List of {String, Int2} |
| `Editor.App/PreferencesDialog.cppm:390` | ShortcutCaptureButton | key chord | key-binding kind |
| `Editor.App/SettingsDialogImpl.cpp:64` | EditText | String settings | String |
| `Editor.App/SettingsDialogImpl.cpp:153` | ComboBox from IsEnum | enum | Enum |
| `Editor.App/SettingsDialogImpl.cpp:169` | CheckBox (no text) | bool | Bool |
| `Editor.App/SettingsDialogImpl.cpp:177` | NumericField u32 honouring `range` | u32 | Int with range |
| `Editor.App/SettingsDialog.cppm:78` | ComboBox MSAA | enum-like u32 | Enum |
| `Editor.App/ExportDialogImpl.cpp:82` | ComboBox | enum | Enum |
| `Editor.App/ExportDialogImpl.cpp:111` | NumericField 0 decimals ..16384 | size | Int / Int2 |
| `Editor.App/ExportDialogImpl.cpp:415` | EditText preset name | String | String |
| `Editor.App/ExportDialogImpl.cpp:420,425,430` | ComboBox template/platform/config | choice | Enum (dynamic choices) |
| `Editor.App/ExportDialogImpl.cpp:437,443` | EditText player / subdir with placeholders | String/path | String with placeholder |
| `Editor.App/ExportDialogImpl.cpp:456` | EditText "files separated by ;" + "Add Files..." | list of paths in one string | List of paths |
| `Editor.App/ExportDialogImpl.cpp:487,491,498,527,543,558,564` | CheckBox with text | bool | Bool |
| `Editor.App/ImportDialog.cppm:68,83,574` | CheckBox (select all / per file) | bool | Bool / list selection |
| `Editor.App/ImportDialog.cppm:93` | EditText asset name | String | String |
| `Editor.App/ImportDialog.cppm:233,746` | CheckBox per importer toggle | bool (only bools are exposed) | Bool |
| `Editor.App/ImportDialog.cppm:601` | ComboBox importer choice | choice | Enum |
| `Editor.App/ProjectManagerView.cppm:803,814` | EditText name, directory + Browse | String/path | String / path field |
| `Editor.App/AssetCreateDialog.cppm:76` | EditText name | String | String |

### 7.2 Pages and panels
| Location | Field | Value | Could be |
|---|---|---|---|
| `Editor.Scene/InspectorViewImpl.cpp:197,235` | EditText + CheckBox grid | collision matrix | mask/matrix kind |
| `Editor.Audio/SoundCuePage.cppm:152` | NumericField per clip "weight" 0..100 | f32 | Float with range (in list element) |
| `Editor.Audio/SoundCuePage.cppm:180` | Button that CYCLES the mode on click | enum (3 values) | Enum |
| `Editor.Audio/SoundCuePageImpl.cpp:175` (calls `SoundCuePage.cppm:189-192`) | NumericField 0..4, 2 decimals, "pitch min/max", "vol min/max" | f32 pairs | min/max range kind |
| `Editor.Audio/AudioClipPage.cppm:97` | Slider 0..1 audition volume (not persisted) | f32 | toolbar Range |
| `Editor.Particles/ParticleEffectPageImpl.cpp:656` | Slider 0..3 simulation speed + label | f32 | toolbar Range |
| `Editor.Animation/AnimationClipPageImpl.cpp:158` | Slider 0..1 time scrub | f32 | timeline (not a property) |
| `Editor.Play/GamePageImpl.cpp:1249` | ComboBox resolution | choice | Enum |
| `Editor.Input/InputMapPageImpl.cpp:92` | ComboBox layout | choice | Enum |
| `Editor.Input/InputMapPageImpl.cpp:1136` AddNameField (5 uses) | EditText, commits non-empty only | String | String with validation |
| `Editor.Input/InputMapPageImpl.cpp:1160` AddNumber (11 uses: Priority, Seconds, Dead zone, Scale, region x4, Radius, Sensitivity, Gravity, Curve) | small label + NumericField 64dp, step 1 or 0.05 | f64 / int | Float / Int with range and unit |
| `Editor.Input/InputMapPageImpl.cpp:1186` AddCheck (5 uses) | CheckBox with text | bool | Bool |
| `Editor.Input/InputMapPageImpl.cpp:1201` AddChoice (4 uses) | ComboBox fixed width | enum | Enum |
| `Editor.Input/InputMapPageImpl.cpp` AddKeycap (6 uses) | listen Button | key binding | key-binding kind |
| `Editor.Input/InputMapPageImpl.cpp:378` | EditableLabel | set/action names | EditableLabel |
| `Editor.PropertyAnimation/ClipEditorViewImpl.cpp:191` AddTextField | EditableLabel (double-click to edit) | String path | String |
| `Editor.PropertyAnimation/ClipEditorViewImpl.cpp:211` AddFloatField (uses :252 Length, :758 key time, :785 value, :833 Euler P/Y/R) | EditableLabel + strtof | f32 | Float (scrub), Euler kind |
| `Editor.PropertyAnimation/ClipEditorViewImpl.cpp:1000` | CurveCanvas | curves | Curve |
| `Editor.Audio/BusLayoutPageImpl.cpp:209`, `Editor.Animation/SkeletonPageImpl.cpp:88` | EditableLabel tree rows | names | EditableLabel |
| `Editor.Terrain/ToolPanelsImpl.cpp:197,244,321`, `Editor.App/ViewModeTogglesImpl.cpp:21` | SegmentedToggle | enum | segmented Enum |
| `Editor.App/LogView.cppm:501`, `Editor.Script/ScriptPage.cppm:243` | ToggleButton chips | bool filter | toggle chip |
| `Editor.Shaders/ShaderPageImpl.cpp:123`, `Editor.Script/ScriptPage.cppm:265` | EditText as read-only error view | text | read-only multiline text |

Search/filter boxes (EditText) are not value editors but share the control: `Editor.Scene/HierarchyView.cppm:83`,
`EntityPickerDialog.cppm:54`, `ComponentPickerDialog.cppm:64`, `Editor.App/AssetsView.cppm:167`, `LogView.cppm:89`,
`PathPickerDialog.cppm:55`, `CommandPalette.cppm:45`, `AssetPickerDialog.cppm:62`, `Editor.Script/ScriptApiBrowserImpl.cpp:200`.

---

## 8. New GUI equivalence map

| Old kind | New GUI equivalent | Notes |
|---|---|---|
| BoolEditor | `BoolPropertyEditor` | tri-state mixed, disabled when RO |
| IntEditor | `NumberPropertyEditor` (integer) | scrub (Shift fine, Ctrl coarse), expressions, snaps to step, Escape cancels scrub, wheel left to grid |
| FloatEditor | `NumberPropertyEditor` | decimals from step |
| RangeEditor | none (bounded NumberPropertyEditor) | `Slider` widget exists; no `slider` hint editor |
| StringEditor | `StringPropertyEditor` | Escape restores; no multiline editor (TextArea exists) |
| EnumEditor | `EnumPropertyEditor` | readable names, type-ahead |
| (none) | `FlagsPropertyEditor` | popup of check boxes, per-bit mixed |
| Float2/3/4Editor | `FloatsPropertyEditor` (hints Float2/3/4) | per-axis gesture and mixed |
| Transform rotation (Float3 Euler) | `FloatsPropertyEditor` on Quaternion | Pitch/Yaw/Roll, step 1 |
| ColorEditor + ColorPicker dialog | `ColorPropertyEditor` + `PropertyColorPopup` | swatch + hex on row, non-modal |
| Material colour + "Intensity" row; HDRColorPicker | `HdrColorPropertyEditor` + `StopsPropertyEditor` | intensity in EV stops |
| CurveFieldEditor | `CurvePropertyEditor` | popup, commit once |
| GradientFieldEditor | `GradientPropertyEditor` | popup |
| ContainerListEditor | `ListPropertyEditor` + element rows | no asset drop-append, no slot rows |
| ResourceRefEditor / AssetPickerSlot / CompactAssetSlot | none | falls to UnknownPropertyEditor |
| Entity ref | none | |
| Guid | none | |
| ButtonEditor | none | |
| NoticeEditor | none | |
| CollisionMatrixEditor | none | |
| Bone name picker | none (could be dynamic Choices) | |
| ShortcutCaptureButton / InputMap keycap | none | |
| SegmentedToggle | none (`RadioButton` group, `TabBar`) | |
| EditableLabel | `EditableLabel` (GUI) | has a validator with `.invalid` mark |
| File path field | none | |
| Grid: changed marker, revert, copy/paste, search, label drag, mixed | present in new grid only | |

---

## 9. Problems a redesign should fix (cross-cutting)

1. No mixed-value support anywhere in the editor today; multi-selection edits only the primary entity.
2. No changed-from-default or prefab-override marker per field; prefab override is one component-level button.
3. Read-only is invisible: null setter, live control, value snaps back each frame.
4. No validation feedback at all: invalid text silently reverts (NumericField, Guid text, InputMap names, ClipEditor strtof).
5. Out-of-range input silently clamps; integer widths wrap; ranges are never displayed (no min/max hint, no tick).
6. No units: radians shown raw for angles (spot cone 0..1.55, particle cone 0..3.1416, a 0..6.2832 range), units
   embedded in label text on some pages, none on others.
7. No drag-to-scrub on any numeric field in the editor (old NumericField drag selects text); spinners take width.
8. Wheel on focused numeric fields changes values while the inspector also scrolls (old); new grid gives the wheel to the grid.
9. Escape cancel missing on vector, range, enum, slots; Tab between vector axes splits one edit into several gestures.
10. Undo has no gesture boundary: consecutive edits of one field merge into one undo step (merge-with-top by key).
11. Colour editing is modal and mouse-only; no hex visible on the row; HDR and noAlpha hints ignored.
12. Particle gradient stop colours cannot be edited (OnStopColorRequested unhandled); curve/gradient rows do not refresh on undo.
13. Asset pick is a modal dialog only; no inline search, no recents, no keyboard path; five spellings of empty.
14. Row presentation inconsistencies: label 40% fixed, no resize; checkbox labels left in the grid but right of the
    box in dialogs; script property labels are raw names (not prettified), enum items raw names.
15. Five independent reflection-to-widget mappings with different coverage (see inconsistencies 4).

---

## 10. Inconsistencies (same value type, different editing)

1. Rotation / angles: Transform rotation = Float3Editor X/Y/Z degrees clamped -360..360, step 1
   (`InspectorViewImpl.cpp:447`); clip editor = EditableLabel P/Y/R degrees (`ClipEditorViewImpl.cpp:833`);
   unused `QuaternionField` X/Y/Z; new grid Pitch/Yaw/Roll; reflected angle fields are raw RADIANS with sliders
   (`Engine.Render/RenderComponentsImpl.cpp:203,207,531`) and particle cone angle radians (`ParticleEffectPageImpl.cpp:257`).
2. Ranged numbers: RangeEditor slider (inspector f32, Materials, Texture anisotropy) vs bounded FloatEditor without
   slider (Fonts 8..128 and 1..32, Terrain tile 0.1..8192) vs SettingsDialog NumericField honouring `range` on u32 while
   the inspector ignores `range` on every integer; Texture/Font pages re-type ranges their own reflection declares.
3. Floats precision and step: 2 decimals (inspector FloatEditor, RangeEditor), 3 (FloatN, script, behaviour interval),
   4 (generic form); step 0.1 / 0.05 / 0.02 / 0.01 by caller.
4. Reflection mapping duplicated five times with different coverage: `SceneInspectorView::BuildPropertyRow`
   (all kinds), `SettingsRows::BuildRow` (no ints, strings, Float2, Float4), `SettingsDialogImpl.cpp` (String/enum/bool/u32
   with hand-built controls), `AssetFormPageImpl.cpp` (scan kinds), new `ReflectedSource`.
5. Read-only display: StringEditor with null setter (Texture, Font, Image, Heightfield, Particles, Generic Blob) vs
   NoticeEditor (scene) vs read-only EditText error views (Shader, Script) vs disabled controls (new grid).
6. Asset references: 17 hardcoded `Ref<T>` branches with literal type lists in the inspector vs `SourceAssetTypesOf`
   in settings rows vs "any asset" in the generic form; empty text "(none)", "None", "(built-in)", "(runtime)", "(missing)", "Default".
7. Numeric input widgets: NumericField with spinners (Float/Int editors, dialogs), without spinners (FloatN), Slider +
   label readout (Preferences UI scale), EditableLabel + strtof (clip editor), EditText parsed as int (MCP port).
8. Enums: ComboBox (grid, dialogs), cycling Button (SoundCue mode `SoundCuePage.cppm:180`), SegmentedToggle (tool
   panels, view modes), raw names (old) vs readable names (new); flag enums only in the new grid.
9. Key bindings: `ShortcutCaptureButton` (Preferences) vs InputMap `AddKeycap` with page-level listen state; different
   look, different cancel/clear keys.
10. HDR colour: Material = ColorEditor + separate linear "Intensity" FloatEditor row 0..1e6 (`MaterialPageImpl.cpp:758,769`);
    reflected `hdrColor` Float4 = four plain numbers; unused `HDRColorPicker` = picker + multiplier; new grid = swatch +
    EV stops on one row.

Also: min/max pairs are two separate rows in Particles ("X min" / "X max", "start" / "end",
`ParticleEffectPageImpl.cpp:201-228`) but four inline fields on one row in SoundCue; bools are a bare checkbox in the
grid but a labelled checkbox in every dialog; staged-until-Save dialogs mix in immediately-applied checkboxes.
