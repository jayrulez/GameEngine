# Structured scene authoring tools - entity and component operations for agents

> ARCHIVED 2026-11-02, partly built; what it still owes is in Documentation/Plans/weekly_backlog.md
> ("Left over from the specs archived 2026-11-02"). Non-authoritative: the original spec, kept as
> the record of what was built and why; the code and the tests are the truth.

> STATUS: PROPOSED 2026-09-28. Sized L. Written from the Sedulous side after its sync of the
> engine composition and the scene format reference, to be built here first and ported. It
> needs one ruling before P2 (see "The ruling this needs"). Every citation below was checked
> against master at 122035b2. Read CONVENTIONS.md first. Incorporates editor-actions.md
> layer 7 (PROPOSED 2026-09-27); how, is in "Relation to editor actions layer 7".

## Goal

An agent builds and changes a scene or prefab by OPERATIONS - create an entity, add a
component, set its fields, parent it, instance a prefab, attach a script with overrides -
instead of regenerating the scene's whole XML. The operations work headless on the stored
source (files are truth) and, once ruled, live on an open scene page as undoable edits. One
operation vocabulary, two executors, the same answers.

Not goals:

- Replacing `scene_write`. Whole-document writes stay for bulk authoring and for anything the
  operations do not cover.
- A new wire format or new component behaviour. The operations write through the existing
  managers, commands and reflection.
- Other page kinds' structures (particle emitters, animation graph nodes, material graphs).
  Those are the page leftovers of editor-actions layer 7 and stay there.

## The problem as it stands

- **Authoring is whole-file.** The headless tools read and write a scene as one XML document
  (`scene_read`, `scene_write`, `scene_validate`; `Code/Editor/Editor.Mcp/SceneTools.cppm:200-422`).
  To add one entity an agent reads the whole stream, edits text, and writes the whole stream
  back. PaperKid (Sedulous, 2026-09) was authored this way: its MainScene is 11 entities in
  846 lines of XML, every change a full rewrite, and before the scene format reference the
  agent wrote a throwaway probe scene to learn the text format at all. `docs://generated/*` and `component_schema` (the reference,
  `SceneReference.cppm`) fixed the learning; they did not fix the granularity.
- **Whole-file writes are the unsafe granularity.** An agent that re-emits 846 lines to
  change one field can drop or reorder records it never meant to touch; `scene_validate`
  catches malformed text, not a silently lost entity. Guids help only if the agent copies
  them faithfully.
- **Live writes stop at one leaf field.** In the editor, `entity_inspect` reads and
  `component_set` writes one property per call through the edit commands
  (`Code/Editor/Editor.Scene/SceneMcpToolsImpl.cpp`, `kSceneLiveToolCount = 9`,
  `SceneMcpTools.cppm:32`). There is no live create, delete, reparent, add-component,
  remove-component or list edit, although every one of them exists as an undoable command on
  `SceneEditContext` for the hierarchy and inspector to use (`EditContext.cppm:110-276`:
  `CreateEntity`, `DestroyEntity`, `RenameEntity`, `ReparentEntity`, `MoveEntityBefore`,
  `SetEntityActive`, `SetLocalTransform`, `SetComponentProperty` with a `ComponentPropertyPath`
  for a struct element in a list, `SetComponentResourceRef`, `SetComponentEntityRef`,
  `SetSceneSettingProperty`, `AddComponent`, `RemoveComponent`, `DuplicateEntity`,
  `PasteEntities`, `SpawnPrefabCommand`).
- **Lists have no edit path anywhere.** Reflection's `ContainerInfo` carries `emplaceDefault`,
  `removeAt` and `moveElement` (`Code/Foundation/Core/Reflection/Reflection.cppm:639-643`),
  used today only by the script backends (`Script.AngelScript`, `Script.Luau`). No editor
  command adds, removes or moves a list element; the inspector edits an element's fields
  (`ComponentPropertyPath`) but not the list's length.

## The earlier ruling, and what changed

`mcp-agent-access.md` (MUTATION SCOPE - DECIDED, :825-850) ruled:

- P2 files-are-truth mutation through the headless write tools; `page_reload` refused while
  the page is dirty. BUILT.
- P2b property writes through the undo path, one labelled step per write. BUILT as
  `component_set`, one grouped step per write under the `mcp` group type (the undo menu does
  not show the label yet).
- "Structural live ops (spawn/add-component/reparent): NOT planned - the file path covers them
  with structurally safer semantics."
- A parked future note: a scene SESSION (open, edits, validate, save) that still lands on
  files-are-truth at save, "if real sessions show scene_write's whole-file granularity is too
  coarse. Revisit with usage evidence, not before."

The evidence now exists (PaperKid, above). This spec answers the parked note in its own terms
for the headless side (P1, operations over the file, validated and saved as one step: the
session collapsed to one call) and asks for the structural live ops as a separate ruling
(P2), because they reverse a decided line.

## Design

### D1. One operation vocabulary

An operation is a JSON object `{op, ...arguments}`. A call carries an ORDERED list of them and
applies them as ONE unit: all or none. Entities are addressed by guid; an operation that
creates an entity may name it with `as: "$hero"`, and a later operation in the same call may
use `"$hero"` wherever a guid goes. The result returns every created guid by its `$name`.

Entities:

| op | arguments | executes through |
|---|---|---|
| `entity.create` | `name`, `parent?`, `before?`, `transform?`, `active?`, `as?` | `CreateEntity` (+ `SetLocalTransform`, `SetEntityActive`, `MoveEntityBefore`) |
| `entity.delete` | `entity` (its subtree goes with it) | `DestroyEntity` |
| `entity.rename` | `entity`, `name` | `RenameEntity` |
| `entity.reparent` | `entity`, `parent` (empty guid = root), `before?` | `ReparentEntity`, `MoveEntityBefore` |
| `entity.setActive` | `entity`, `active` | `SetEntityActive` |
| `entity.setTransform` | `entity`, `position?`, `rotation?` (quaternion `[x,y,z,w]` or euler degrees `{x,y,z}`), `scale?` | `SetLocalTransform` |
| `entity.duplicate` | `entity`, `as?` | `DuplicateEntity` |
| `prefab.instantiate` | `prefab` (asset guid), `parent?`, `transform?`, `as?` | `SpawnPrefabCommand` |

Components:

| op | arguments | executes through |
|---|---|---|
| `component.add` | `entity`, `type`, `fields?` | `AddComponent` at defaults, then `component.set` of `fields` |
| `component.remove` | `entity`, `type` | `RemoveComponent` |
| `component.set` | `entity`, `type`, `fields` | per field: `SetComponentProperty` / `...ResourceRef` / `...EntityRef` / `...Raw` |
| `list.insert` | `entity`, `type`, `field`, `index?` (default append), `value?` | new: `ListElementCommand` (D4) |
| `list.remove` | `entity`, `type`, `field`, `index` | new: `ListElementCommand` |
| `list.move` | `entity`, `type`, `field`, `from`, `to` | new: `ListElementCommand` |
| `settings.set` | `system`, `fields` | `SetSceneSettingProperty` / `...ResourceRef` |

Scripts:

| op | arguments | executes through |
|---|---|---|
| `script.attach` | `entity`, `script` (asset guid), `enabled?`, `overrides?` (`{name: value}`) | adds a `ScriptComponent` if absent, appends a behaviour |
| `script.override` | `entity`, `behavior` (index), `name`, `value` (null clears) | sets or removes one override |

- **`type` is the wire name** (a record's `type`, `"light"`, `"physics.RigidBody"`), the same
  name `component_schema` and `docs://generated/SceneSchema.json` key components by, so an
  agent reads the schema and writes with its names. The reflected type name is accepted too,
  as `component_schema` accepts it (`FindSchemaEntry`).
- **Field keys are the schema's keys**, and values take the shapes `entity_inspect` reads and
  `component_set` writes: numbers, booleans, strings, guid strings, `[x,y,z]` vectors,
  `[r,g,b,a]` colours, an enum by case name or number, a `Ref<T>` by asset guid, an
  `EntityRef` by entity guid or `$name`. A list field given a JSON array REPLACES the list;
  the `list.*` ops edit it in place. A key the schema lists under `unreflected` (its data
  lives outside a reflected field, a spline's points) is refused with that reason: that
  component needs its own ops, a later item.
- **Script overrides by NAME.** The tool hashes the name (`ScriptPropertyNameHash`) and checks
  it against the cooked class's harvested properties when the class is cooked (refused with
  the declared names when it names none, the check the Sedulous PaperKid test now makes); the
  value's kind comes from the declaration, so the agent writes `{"target": "$camera"}` and
  never a hash or a kind code.

### D2. The headless executor: `scene_edit`, files are truth

`scene_edit {guid, ops[]}` on both hosts (Editor.Mcp, beside `scene_write`; `prefab_edit`
mirrors it with the single-root rule, as `prefab_write` does):

1. Load the stored stream into a scratch scene of the FULL composition
   (`engine::AddAllSceneManagers`), exactly as `scene_validate` loads it.
2. Apply the operations in order on the scratch scene through the same functions the live
   executor's commands call (D5), resolving `$names` as they are created. The first failing
   operation refuses the whole call with its index and reason; nothing is written.
3. Validate the result as `scene_validate` would (the same report), save it through
   `SaveScene` (the editor's own writer, so the text is the editor's text), and fire
   `ProjectSession::onAssetWritten` (`McpSession.cppm:33-37`).
4. Return `{guid, created: {"$name": guid}, entityCount, warnings}`.

On the editor host `onAssetWritten` reaches `EditorContext::NotifyAssetExternallyModified`
(`Editor.Core/ContextImpl.cpp:270`): an open, clean scene page reloads; a DIRTY one refuses the
refresh and warns (`Editor.Scene/ScenePageImpl.cpp:597-616`). `scene_edit` refuses up front
when the scene is open and dirty on the editor host ("the user has unsaved changes on this
scene; use the live tools or ask them to save"), so it never writes a file the page would then
refuse to show. This is the parked "scene session" collapsed into one call: open, edits,
validate, save, with files-are-truth intact.

Read side: `scene_outline {guid}` returns the entity tree (guid, name, active, parent, the
component wire names per entity) without the field data, so an agent navigates a large scene
without reading its XML. `scene_read` stays for the full text.

### D3. The live executor: `page_edit`, one undo step per call (needs the ruling)

`page_edit {page?, ops[]}` on the editor host, beside the live tools (`page` addresses as
`ResolveScenePage` does, `SceneMcpToolsImpl.cpp:35-70`: a guid, else the active scene page):

- The call runs inside ONE command group on the page's stack (`Editor.Core/Command.cppm:10,
  53-60`), opened, closed and locked as `component_set` does its write
  (`BeginGroup(u8"mcp")`, `EndGroup`, `LockGroup`, `SceneMcpToolsImpl.cpp:976-979`), so the user
  undoes an agent's call as one step. Showing the `mcp` group type in the undo menu, so the user
  sees the step was the agent's (mcp-agent-access P2b asked for that label), is part of P2.
- Each operation is the command the hierarchy or inspector already runs (D1's tables). A
  failing operation undoes the group's applied commands and refuses the call: no half-applied
  step reaches the stack (mcp-agent-access P2b's rule).
- Refused while the page simulates (Simulate locks edits, `SceneMcpToolsImpl.cpp:711-714`).
  Nothing saves; the page is dirty as after a user edit, and `action_execute file.save` or the
  user saves it.
- Runs under the unattended scope (editor-actions layer 5, `UnattendedDialogs`): an operation
  whose command raises a dialog does not block the host.
- `page_outline {page?}` is `scene_outline` over the live scene, and `entity_inspect` stays the
  per-entity field read.

### D4. The list element command

`ListElementCommand` (Editor.Scene, with the edit commands) edits a reflected container
property through `ContainerInfo` (`emplaceDefault`, `removeAt`, `moveElement`), the component
addressed as every property command addresses it. Undo is exact the simple way: the command
captures the component's blob before (the manager's `WriteComponent`, as
`PasteComponentCommand` captures `m_previous`, `EditContext.cppm:636-714`) and restores it
through `ReadComponent`. The inspector gains the same add, remove and reorder on its list rows
through this command, so the operation is not an MCP-only path.

### D5. One apply function per operation, two executors

Each operation's body is written once, over a `scene::Scene&` and the arguments, in
Editor.Scene's edit layer; the live commands call it in `Execute`, the headless executor calls
it on the scratch scene. The headless side therefore needs Editor.Scene's operation layer
without its pages and views: those functions live in a small `editor.scene:operations`
partition with no UI imports, and Editor.Mcp imports that partition (the D6 dependency check
of scene-format-reference.md applies: Editor.Mcp stands on Editor.Project and must not pull in
Editor.Core's UI half or the pages). If the partition cannot be kept free of the page layer,
the operation bodies move to a library of their own (`Editor.SceneEdit`) under both.

### D6. What the answers say

Every refusal names the operation's index, the op, and what to do: an unknown wire name lists
the known ones (as `component_schema` does), an unknown field lists the component's keys, a
read-only field says so (the `[ReadOnly]` flag `component_set` honours,
`SceneMcpToolsImpl.cpp:881`), an `unreflected` key says where its data lives. A successful
call returns what it created and the resulting counts, so an agent never has to re-read the
scene to learn a guid.

## Relation to editor actions layer 7

Layer 7 (editor-actions.md:203-315) moves the asset browser onto the action registry over a
PUBLISHED asset selection, adds the page leftovers (`scene.entity.copyId`, the particle
emitter and animation graph node actions), and rolls the page toolbar out. This spec builds
on it and does not overlap it:

- **Actions stay nullary, operations take parameters.** The registry's rule
  (editor-actions.md:85-88: an action is nullary, "anything that takes a parameter is a tool,
  not an action") puts `scene.entity.create/duplicate/delete/copyId` over the SELECTION in the
  registry and this spec's `entity.create {name, parent, ...}` over EXPLICIT guids in tools. The
  two execute through the same `SceneEditContext` commands, so an action and an operation
  that do the same thing leave the same undo step.
- **The published selections are the handoff.** Layer 7 publishes the asset selection into
  `EditorContext::AssetSelection()` as the hierarchy already publishes `EntitySelection()`.
  `selection_get` reports both once layer 7 lands (entities per page, assets editor-wide), and
  the live operations accept `"$selection"` (every selected entity, in order) and
  `"$primary"` as entity arguments and `"$asset"` (the primary selected asset) wherever an
  asset guid goes, so "instantiate the prefab I selected under the entity I selected" is one
  `page_edit` call. Without layer 7 the asset side of that is absent and nothing else changes.
- **Rename.** Layer 7 keeps the hierarchy's and browser's Rename hand-bound because they start
  an inline edit. `entity.rename` takes the name as an argument, so it needs no inline edit;
  it runs `RenameEntity`, the command the inline edit commits.
- **The page leftovers are not this spec's.** Particle emitters and animation graph nodes are
  their pages' own structures with their own services (`IParticleEditorPage`,
  `IAnimationGraphPage` in layer 7); structured operations over them would follow this spec's
  shape later, over those services.
- **The page toolbar** needs nothing from this spec; a `page_edit` call marks the page dirty,
  and the toolbar's save and undo reflect it through their pulled state as for any edit.

Landing order between the two: independent. P1 below needs nothing from layer 7; P2 benefits
from landing after it (the `$asset` form).

## The ruling this needs

P2 reverses "Structural live ops: NOT planned" (mcp-agent-access.md:838-839). The case for it:

- The headless P1 covers every operation but cannot touch a scene the user has open and
  dirty, which is exactly when an agent working WITH the user is asked for a change ("add a
  light over there"). Today the agent must refuse or ask the user to save first.
- The safety argument of the original ruling (files are structurally safer) is kept: the
  live executor adds no new mutation path, it calls the commands the hierarchy and inspector
  already run, grouped, labelled, undoable, and refused during Simulate.
- The parked "agent as a collaborative peer on the live document" note named P2b's undo
  writes as its embryo; `page_edit` is that embryo grown to the operations the editor's own
  surfaces already have, not a new document model.

Options: (a) rule P2 in as specified; (b) P1 only, and the agent asks the user to save before
a headless edit of an open scene; (c) P2 limited to additive operations (create, add,
instantiate, set) with deletes left to the user.

## Phases

- **P1: the vocabulary and the headless executor** (Editor.Scene operations partition,
  Editor.Mcp; M). D1 minus the list ops and scripts, D2, D5, D6; `scene_edit`, `prefab_edit`,
  `scene_outline`. Tests (Integration.Mcp, over a scratch project):
  - a batch creating a parent and a child with `$names`, adding a light with fields and a
    mesh with a `Ref<StaticMesh>`, reparenting, then reading the stored XML back through
    `LoadScene`: every record present, the guids the result returned;
  - all or none: a batch whose third op names an unknown wire name leaves the file byte
    identical and refuses naming index 2 and the known names;
  - an `EntityRef` field set by `$name` resolves to the created entity;
  - a read-only field refused; an `unreflected` key refused with its reason;
  - `prefab_edit` refusing a second root;
  - on an editor-host fixture, a clean open page reloads after `scene_edit` and a dirty one
    makes `scene_edit` refuse before writing;
  - `scene_outline` over the scene format reference's example scene lists every entity and
    its component wire names.
- **P2: the live executor** (Editor.Scene; M; needs the ruling). D3; `page_edit`,
  `page_outline`. Tests (Editor.Scene.Tests, headless page fixture): one call, one undo step,
  and undo restores the pre-call scene exactly (blob comparison); a failing op mid-call leaves
  the stack and the scene unchanged; refused while simulating; `$selection` and `$primary`
  resolve from the page's entity selection.
- **P3: lists and scripts** (Editor.Scene, Editor.Mcp; M). D4's `ListElementCommand` with the
  inspector's list rows on it; `list.insert/remove/move`; `script.attach`, `script.override`
  with the name-to-hash and the cooked-class check. Tests: each list op and its exact undo; an
  override by name on a cooked class, and the refusal listing the declared names on an unknown
  one; the example's behaviour rebuilt through operations serializes identically to the one
  the scene format reference writes.

Each phase is one commit with its tests, both compilers, then the lanes.

## Acceptance

- An agent adds, changes, parents and removes entities and components in a stored scene
  without emitting its XML, and every call is atomic.
- With P2 ruled in: the same operations on an open scene, one labelled undo step per call.
- The operations and the editor's own surfaces leave identical scenes and identical undo
  steps for the same change.
- `docs://McpGuide.md` names `scene_edit` as the default authoring path and `scene_write` as
  the bulk path; `Scenes.md` points at the vocabulary.

## For the Beef port (Sedulous)

The contract is the vocabulary (D1: op names, arguments, `$name` and the `$selection` forms,
wire names and schema keys), the atomicity, the answers of D6, and the tool names. Sedulous
has the pieces under other names: `SceneEditContext` with the reference and entity-reference
commands (`Editor/Sedulous.Editor.Scene/src/Edit/`), `component_set`'s value shapes in
`Page/ComponentJson.bf`, the schema key join in `Editor.Mcp/src/SceneReference.bf` (a key is
the field name with its first letter lowercased), and `IReflectedList` for reading a list
(`Foundation/Sedulous.Core/src/Reflection/IReflectedList.bf`), which gains insert, remove and
move for D4. Its Editor.Mcp stands on Editor.Project as here, so the D5 dependency question is
the same one.
