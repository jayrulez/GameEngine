# Editor lists and asset slots - one list widget, one asset slot, drop everywhere

> STATUS: PROPOSED 2026-09-28. Sized L. LED BY SEDULOUS (user, 2026-09-28): built there first,
> phase by phase, and ported here from its commits; the Raptor citations below say where each
> piece lands in this tree. Both trees carry the same code and the same inconsistencies. Every citation
> below was checked against master at 122035b2. Read CONVENTIONS.md first. Folds in two
> REMAINING items of reflection-track.md (:449-455): the reflection-first generic asset page
> and the list editor's polish.

## Goal

Every editable list in the editor looks and behaves the same, and every row that names an
asset shows it the same way and takes it by drag and drop as well as by picker. A user who
learned one inspector knows all of them.

Not goals:

- New editing capability. Each list keeps what it can do today; it gains move and remove
  where it lacked them only because the one widget offers them.
- Wire changes. The sound cue keeps its fixed slot count (see D4).
- Selection lists that are navigation (the animation graph's layer and parameter lists in its
  left panel, the particle page's system tree). They select what the grid edits; they are not
  the lists this spec is about, beyond D4's note on their add and delete.

## The problem as it stands

### Lists: one good widget, many hand-rolled ones

`app::ContainerListEditor` (`Code/Editor/Editor.App/ContainerListEditor.cppm:30-37`) is the
good one: one grid row whose view is a header with an add icon over a row per element, each an
`AssetPickerSlot` that fills plus move-up, move-down and remove icon buttons. The generated
inspector uses it for the mesh's materials (`Editor.Scene/InspectorViewImpl.cpp:1763`) and for
reflected containers (`:2709`); the particle page uses it for a system's materials
(`Editor.Particles/ParticleEffectPageImpl.cpp:1631`). reflection-track.md records how it came to be
(:433-445).

Everywhere else a list is hand-rolled, in four different shapes:

| Where | What it is today |
|---|---|
| Script component behaviours, `InspectorViewImpl.cpp:1925,2063,2083` | full-width text button rows "+ Add Behavior", "Move Up", "Remove Behavior"; no move-down |
| Animation clip events, `AnimationClipPageImpl.cpp:593,618` | "+ Add Event" and "Remove Event" text button rows; no reorder |
| Animation graph conditions and blend entries, `AnimationGraphPageImpl.cpp:1547,1585,1682,1710` | `RowButton` text rows "+ Add Entry", "Remove Entry", "+ Add Condition", "Remove Condition" |
| Animation graph layers and parameters, `AnimationGraphPageImpl.cpp:812,842,1203,1347` | left panel "+ Add Layer" / "+ Add Parameter" rows; "Delete Layer" / "Delete Parameter" text rows in the grid |
| Audio bus layout, `Editor.Audio/BusLayoutPageImpl.cpp:285,629` | a plain "+ Add Bus" button and a "Remove Bus" text row |
| Physics collision groups, `InspectorViewImpl.cpp:255` | a plain "+ Add Group" button |
| Input map sets, `Editor.Input/InputMapPageImpl.cpp:484` | a footer "+ Add Set" through the page's own `MakeButton` |
| Terrain paint layers, `Editor.Terrain/TerrainEditorPageImpl.cpp:400` | "+ Add paint layer" through the page's own `addButton` |
| Particle systems, `ParticleEffectPageImpl.cpp:1244,1286,1336,1449` | "Add System" as a grid text row; modules added, deleted and moved only in the tree's context menu |
| Sound cue slots, `Editor.Audio/SoundCuePage.cppm:128` | a fixed number of slots, each label + "Pick..." + "Clear" + weight |
| The generic asset page, `Editor.Generic/AssetFormPageImpl.cpp:463` | array counts shown and skipped: no list can grow or shrink |

The component header already uses icons for its own remove (`InspectorViewImpl.cpp:1196-1197`,
"Remove component"), so a behaviour's full-width "Remove Behavior" row sits beside an icon that
does the same kind of thing for the component it lives in.

### Asset rows: one slot, drop wired in one place

`app::AssetPickerSlot` (`Editor.App/AssetPickerSlot.cppm:38-201`) is a drop target for asset
browser drags (`AssetDragData`), accepting by type name with a hover cue and a rejection callback
- but ONLY once its accepted types are set, and only one caller sets them: the inspector's single
`Ref<T>` rows (`InspectorView.cppm:670` for settings blocks, `:766` for components). Nothing else
in the editor handles an asset drag (`AssetDragData` has no consumer outside the slot and the
browser's cells).

So, drop does NOT work on:

- **Slots that could take it but are never told the types**: every `ContainerListEditor` slot,
  the mesh's material list included (the widget never calls `SetAcceptedTypes`); the script
  behaviour's script (`InspectorViewImpl.cpp:1973`); a script's asset-typed property (`:2324`);
  the material page's texture slots and preview mesh (`Editor.Materials/MaterialPageImpl.cpp:518,
  691`).
- **Rows that are not slots at all**, shown as a label and a button or as a button whose text is
  the value:
  - project settings, 7 asset rows each "Pick..." (`Editor.App/SettingsDialog.cppm:77,111,151,
    192,232,273,314`);
  - the collision shape's source mesh (`Editor.Physics/CollisionShapePageImpl.cpp:104`);
  - terrain's heightfield, weights, base albedo and layer textures (`TerrainEditorPageImpl.cpp:
    257`, `PickReference`);
  - a particle system's texture and mesh, a button reading "(set - click to change)" that never
    names the asset (`ParticleEffectPageImpl.cpp:1564,1597`);
  - the sound cue's clips (`SoundCuePage.cppm:128`);
  - the generic page's guid fields, a text row plus an UNTYPED "Pick <field>" picker
    (`AssetFormPageImpl.cpp:514`);
  - the preview controls: mesh page preview material (`MeshPageImpl.cpp:249`), animation clip and
    graph preview skeleton and mesh (`AnimationClipPageImpl.cpp:143-146`,
    `AnimationGraphPageImpl.cpp:480-483`), the UI theme's preview document
    (`Editor.GameUI/UIThemePage.cppm:179`).

### Entity rows: two pickers

A generated `EntityRef` row is a slot that opens `EntityPickerDialog`, a searchable tree
(`InspectorViewImpl.cpp:2402-2412`). A script's entity-typed property opens a flat context menu
of every entity in the scene (`:2324-2337`). Neither accepts an entity dragged from the
hierarchy.

## Design

### D1. ContainerListEditor is the one list widget

It grows three things, each additive; the current callers keep working:

- **Element content.** Today an element is an `AssetPickerSlot`. It becomes a choice per list:
  an asset slot (as now), an entity slot (D5), or a BODY the caller builds - a
  `Function<void(usize index, ui::FlexLayout& body)>` that fills an expandable per-element
  section with ordinary property rows. The element's move-up, move-down and remove icons stay on
  its header line whatever its content. A behaviour, a clip event, a graph condition, a paint
  layer are bodies.
- **An add menu.** When the caller supplies `OnAddMenu`, the header's add icon opens a menu
  instead of appending: the polymorphic add-by-type reflection-track.md asks for (:453-455,
  category-grouped from `EnumerateDerived`), and the particle modules' "Add Behavior /
  Add Initializer" submenus (`ParticleEffectPageImpl.cpp:1166`).
- **Drop.** An asset-slot list takes its accepted types (`SetAcceptedTypes`, as the slot does);
  a drop on an element's slot assigns that element (`OnAssignSlot(index, guid)`), and a drop on
  the header or the empty list APPENDS (`OnAppendDropped(guid)`), so dragging three materials
  onto a mesh fills three slots. A rejected type reports through the same callback the slot has.

An element label may be supplied (`Element 0` today); the body variant's header shows the
caller's summary ("Behavior: Mover", "Event 2 @ 0.40s").

As built in Sedulous (P0): the property grid's categories are flat, so a BODY element is a grid
section of its own, like a component and like the generated struct-list sections, and its move
up, move down and remove icons sit in that section's header (`ContainerListEditor.ElementActions`,
handed to the grid's category header actions). The list's own row is then its header alone, the
count and the add icon (`ElementsAsSections`), and it still takes an append drop. Slot elements are
unchanged. `ResourceRefEditor` moved to Editor.App beside the slot so every page can use it;
`AssetPickerSlot.cAnyAsset` ("*") is the explicit any-asset type, and no types at all means the
row is no asset drop target (an entity reference); `BindAsset(context, current, assign)` wires
pick, drop, clear, edit and reveal to the one assignment; `EditorContext.AssetNameFor` names an
asset, "(missing)" for a dangling id (the inspector's copy never wrote it).

### D2. Every asset reference is an AssetPickerSlot that accepts drops

- `ResourceRefEditor` takes its accepted asset types at CONSTRUCTION and wires drop itself, so
  a caller cannot build a picker row without drop. The assignment on drop goes through the same
  function as the picker's `OnPicked`, so pick and drop are one code path and one undo step.
  An empty type list means "any asset", for the few rows that genuinely are untyped (the generic
  page until P4).
- The slot shows the asset's name and thumbnail and has its own clear (as the inspector's rows
  do); no page keeps a separate "Clear" button.
- A compact slot variant (the slot at toolbar height, no label column) serves the preview
  controls, so the mesh page's preview material, the clip and graph pages' preview skeleton and
  mesh, and the UI theme's preview document take a drop too. They remain preview state, not
  asset fields.

### D3. Page rows move onto D2

Project settings' seven rows, the collision shape's mesh, terrain's references, the particle
system's texture and mesh (which then NAME their asset), the sound cue's clips, the material
page's slots, the script behaviour's script and a script's asset properties, and the preview
controls: each becomes a `ResourceRefEditor` or compact slot with its types. The pick dialog
each opens today keeps its type filter, which becomes the slot's accepted types.

### D4. Hand-rolled lists move onto D1

- **Script behaviours** become a body list: each behaviour's section holds the script slot, the
  enabled and interval rows and its overrides; add, move up, move down and remove are the
  widget's icons. One undo step per operation, through the component mutation path the section
  uses today.
- **Animation clip events**, **graph conditions** and **blend entries** become body lists.
- **Terrain paint layers** become a body list.
- **Collision groups** (the collision matrix editor) and **input map sets** move their add and
  remove onto the widget's icons.
- **The audio bus layout** is a tree, not a list; it keeps the tree and moves "+ Add Bus" and
  "Remove Bus" onto header icons over the tree and the tree's context menu, as the particle page
  already does for its systems. The particle page's "Add System" grid row goes the same way.
- **Navigation lists** (graph layers and parameters in the left panel, particle systems in the
  tree) keep their shape; their add becomes a header add icon and their delete moves from a
  full-width grid row to the list's context menu and the grid section's header icon, like a
  component's remove.
- **The sound cue** keeps its fixed slot count (a wire property of `SoundCueAsset`); its rows
  become asset slots with weights. Making the slot count variable is a wire change and a
  separate item.

### D5. One entity picker, and entity slots accept hierarchy drags

- A script's entity-typed property uses `EntityPickerDialog` like every other `EntityRef` row;
  the flat menu goes.
- An entity slot accepts a hierarchy row dragged onto it (the hierarchy's `TreeDragData`,
  resolved to the entity guid), with the same hover cue and rejection path as an asset slot, so
  "drag the camera onto the follow target" works in the inspector as it does in any other
  engine's.

### D6. The generic asset page is reflection-first (reflection-track P4)

reflection-track.md's first REMAINING item (:449-452): the generic page stops being
serialize-driven (`AssetFormPageImpl.cpp`, `AssetFormFieldKind` Guid / ArrayCount) and builds
from the asset's reflection, so a `Ref<T>` field is a typed slot with drop (D2), a container is a
`ContainerListEditor` (D1), and an enum is a dropdown by name. Until it lands, P1 gives its guid
fields an untyped slot with drop.

### D7. The rule, written down

CONVENTIONS.md gains, under "Architecture rules that recur": "An editable list in the editor is
a `ContainerListEditor`; an asset reference is an `AssetPickerSlot` (through `ResourceRefEditor`
or the list widget) with its accepted types set, so it takes a drop. No full-width text button
rows for add, remove or reorder." That keeps the next page from rolling its own.

## Relation to other specs

- **reflection-track.md**: its REMAINING items 1 and 2 are D6 and D1 here; stamp them there when
  this lands.
- **editor-actions.md layer 7**: the asset browser's drag source (`AssetCell`) is unchanged;
  layer 7 moves its menus onto actions. No overlap. A slot accepting a drop is not an action.
- **scene-authoring-tools.md** (PROPOSED): its D4 list element command and this spec's D1 list
  operations must be the SAME command, so an agent's `list.insert` and a user's add icon leave
  the same undo step. Whichever lands first defines it; the other reuses it.

## Phases

- **P0: the widgets** (Editor.App; M). D1's element bodies, add menu and list drop; D2's
  constructor-typed `ResourceRefEditor` with drop and the compact slot. Tests (Editor.App.Tests,
  with a synthetic `AssetDragData` as `AssetPickerSlotTests` already builds): a drop on an
  element assigns it, a drop on the header appends, a wrong type is rejected and reported, the
  add menu's choice appends that type, a body list's move and remove reach the callbacks with the
  right indices, a `ResourceRefEditor` drop and pick call the same assignment.
- **P1: drop everywhere** (every page; M). D3 across the listed rows, the generic page's untyped
  slots. Tests per page on a headless fixture: a drop on each converted row assigns the asset
  and is one undo step; the particle rows name their asset.

  As built in Sedulous (P1): every converted row is a `ResourceRefEditor` bound with `BindAsset`
  (the project settings rows without edit and reveal; the preview bars through
  `CompactAssetSlot`, a caption over the same row). The pages build only with a render host, so
  there is no headless page fixture: the widgets' drop and assignment are proven in
  Editor.App.Tests (P0), and the inspector's generated asset row and asset list are proven end
  to end (a drop assigns, a slot drop assigns, a list drop appends, each one undo step, a wrong
  type changes nothing). Two buttons stay buttons, being verbs and not references: the UI theme
  page's "Preview Document..." (copies a document's markup once) and the property animation
  component's "Open Clip" (opens a document).
- **P2: the lists** (Editor.Scene, Audio, Input, Terrain, Physics; L). D4. Tests: each converted
  list's add, move and remove through the widget, one undo step each, the behaviour list's body
  rows editing the behaviour they belong to after a move.

  As built in Sedulous (P2):
  - Section lists: script behaviours ("Behavior N - <class>", move and remove), clip events,
    blend entries, transition conditions and terrain paint layers (the terrain's in a grid of
    their own, each layer's five maps and tiling in its section). `ElementActions` with a null
    move is remove alone: event, entry and condition order means nothing, and a paint layer's
    index is its weight channel.
  - A script class dropped on the behaviour list's header appends a behaviour running it. The
    behaviour's Enabled and Update Interval rows gained refreshers: a move of two behaviours of
    one class, or an undo, left them showing the old values.
  - `ListHeader` (Editor.App): a title and the add icon over a navigation list or tree. The
    graph's layers and parameters (right click Delete; the section's header remove icon), the
    bus tree (context menu Add Bus and Remove Bus; the Bus section's remove icon) and the
    particle systems (the General section's delete icon; the effect node shows the count).
  - The collision matrix's add is an icon in the header corner, its remove an icon on the last
    group. The input map's add and remove verbs are icons, "Add Set" a header over the sets; the
    priority steppers stay (a value, not the list).
  - `PropertyGrid.GetCategoryHeaderActions`, so a test drives a section's icons.
  - Not in this inventory, left as they are: the export dialog's per profile Edit, Duplicate
    and Delete, its templates' Remove, the project manager's recent Remove.
  - D7 has no home in Sedulous (no conventions file); it lands in CONVENTIONS.md on the port.
- **P3: entities** (Editor.Scene; S). D5. Tests: a script entity property picks through the
  dialog; a hierarchy row dropped on an `EntityRef` slot assigns it; a non-entity drag is
  refused.

  As built in Sedulous (P3): `TreeDragData` carries an item kind, id and name, filled by the
  tree's owner through `DraggableTreeView.OnDecorateDragData` (the hierarchy names the row's
  entity). `AssetPickerSlot.cEntity` is the accepted type of an entity reference, so an entity
  slot or entity list takes a hierarchy row through the same drop, hover ring and rejection as
  an asset (an asset slot, `cAnyAsset` included, refuses an entity, and an entity slot an
  asset); `DescribeDrag` and `Accepts` are shared with the list widget.
  `ResourceRefEditor.BindEntity` is `BindAsset`'s sibling: the caller's picker, a namer, drop
  and clear as one assignment. The generated entity row, the entity list and the script
  entity property use it; the script property's flat menu is gone for `EntityPickerDialog`.
  Tested: the slot and list take a hierarchy row and refuse an asset and a bare row, the
  hierarchy names the dragged entity, a drop on the generated row is one undo step. The script
  property's dialog pick is not driven headless (the dialog needs a UI context).
- **P4: the generic page** (Editor.Generic; M). D6. Tests: a generic asset with a `Ref<T>`, a
  list and an enum renders a typed slot, a list widget and a dropdown; each edits and undoes.
  Sedulous (P4): DEFERRED (user, 2026-09-28). Sedulous assets carry no run-time reflection
  (Documentation/RuntimeReflection.md), so "builds from the asset's reflection" has no
  equivalent there yet; the generic page keeps P1's untyped slots with drop. The route on the
  table: `[Serializable]`'s comptime generator also emits a per type field table (key, `Ref<T>`
  target, enum cases, list element) that the page joins to its scan by key.
- **D7** lands with P2, when the last hand-rolled list is gone.

Each phase is one commit with its tests, both compilers, then the lanes. The user does the
visual verification; each phase hands over a build and the list of pages to look at.

## Acceptance

- No full-width text button row adds, removes or reorders anything in the editor.
- Every asset reference row, in every inspector, page, dialog and preview control, accepts a
  drop of an asset of its type and refuses one of another type with the same cue.
- Every entity reference row opens the same picker and accepts a hierarchy drag.
- The generic page edits lists and typed references.

## For the Beef port (Sedulous)

The same survey was made there first and found the same set under the same names:
`ContainerListEditor.bf` and `AssetPickerSlot.bf` (Editor.App), `InspectorSection.bf:254` as the
one drop wiring, `SceneInspectorViewScripts.bf` for the behaviours and the script entity menu,
`GenericAssetEditorPage.bf`, `ProjectSettingsDialog.bf`, `SoundCueEditorPage.bf`,
`TerrainEditorPageFields.bf`, `ParticleEffectEditorPageInspector.bf:111,129`. Its generated
inspector also has struct-list sections (`InspectorSection.SlotSections`), which D1's body
elements should absorb so the two trees keep one list shape.
