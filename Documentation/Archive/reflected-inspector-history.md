# Reflected inspector (generic property + resource-ref widgets)

> ARCHIVED 2026-11-02, obsolete: its subject was replaced or removed. Non-authoritative: the
> original spec, kept as the record of what was built and why; the code and the tests are the truth.

> STATUS: SPEC PREPARED 2026-08-16 (user-requested after the PropertyAnimator
> clip-picker gap). Implementation NOT scheduled. Sits on the reflection track
> (Documentation/Archive/reflection-track-history.md) - read that first; this spec is the
> INSPECTOR consumer of the attribute + type metadata that track already
> registers. Memory `inspector-ref-picker-table` is the incident history this
> spec exists to close for good.

## Goal

Make the component inspector fully reflection + attribute driven, so adding a
component - or a new resource type, or a new value type - requires ZERO edits to
the inspector. The recurring class of bug (a `resource::Ref<T>` field that
silently renders NO widget until someone hand-adds a dispatch row) disappears by
construction: an unrecognized property becomes a LOUD "unsupported" row, never
nothing.

Non-goal: a new widget library. Every widget already exists (the
`ui::toolkit::PropertyEditor` family). This is about DISPATCH: replacing a
hand-maintained per-type table with reflection queries.

## The problem (current state)

`SceneInspectorView` (`Code/Editor/Editor.Scene/InspectorViewImpl.cpp`) has THREE
parallel, hand-maintained `if (prop.type == &TypeOf<...>())` dispatch chains that
have already DRIFTED apart:

- **Components** - `BuildPropertyRow` (lines ~1070-1491): 14 resource-ref branches
  (`Ref<StaticMesh>` .. `Ref<UITheme>`, and now `Ref<PropertyAnimationClipResource>`),
  each calling `BuildResourceRefRow<T>(id, type, prop, category, {u8"<Asset>Name"})`
  with a hard-coded asset-name list; then `EntityRef` (its own picker); then the
  value fallbacks (`IsEnum`->`EnumEditor`, then `f32`/`bool`/int/`String`/`Float3`/
  `Color`/`Float2`/`Float4` by TypeInfo identity, `range` attr picking RangeEditor
  over FloatEditor); the final "nothing matched" case is a SILENT skip (line ~1490).
- **Scene-system settings** - `BuildSettingRow` (~613-816): a near-duplicate with
  its own `BuildSettingResourceRefRow<T>` chain (only `Ref<Texture>` +
  `Ref<ScriptClass>`) and a SUBSET of the value editors - it lacks int/String/
  Float2/Float4 (the chains have drifted).
- **Containers / nesting** - in `BuildComponentSection` (~880-935): `IsContainer`
  -> list editor, `IsNested` -> skip. The materials LIST is hard-specialized to
  `Ref<Material>` even on the "reflected" path.
- Bespoke special cases keyed by concrete type: the collision-matrix editor
  (`PhysicsSceneSettings`), a RigidBody notice.

Two failure modes, both structural:

1. **Silent omission.** A new `Ref<T>` field renders nothing until someone adds a
   row + an `import` + a `Foundation::*.Resource` link in the Editor.Scene CMake.
   This has bitten at least four times (UI document/theme, AudioSource clip,
   RigidBody shape/material, PropertyAnimator clip) - three separate subsystem
   tracks made the identical omission. The property just vanishes; no error.
2. **Edit-to-extend.** Every new value type needs a new branch in one god
   function that already every editor lib must reach around.

Root cause: the picker needs two things plain reflection does not surface -
(a) a type-erased get/set of the Ref's Guid + its product type, and (b) the
resource-type -> pipeline-asset-name mapping used to filter the picker. Both are
supplied by hand in the table today.

## What already exists (build on, do not reinvent)

- **`resource::Ref<T>`** (`Foundation/Resource/ResourceModule.cppm`) stores a plain
  `Guid id` (the serialized identity) plus a bound `Proxy<T>`. `Bind(manager)`
  attaches the proxy.
- **Type-erased bind:** `ResourceManager::Bind(const TypeInfo& productType, const
  Guid& id) -> RefPtr<ResourceHandle>` already exists; `Bind<T>(id)` is just
  `Bind(T::StaticType(), id)`. So binding without the static `T` is already
  possible.
- **Builder registry pairs product + asset:** every `IAssetBuilder`
  (`Pipeline/Pipeline.Core/Asset.cppm`) exposes `AssetType()` AND `ProductType()`.
  The `BuilderRegistry` indexes by asset (`Find(assetType)`, `FindByTypeName`) but
  has NO reverse product-type index - so the resource-type -> asset-name mapping is
  DERIVABLE (iterate builders comparing `ProductType()`), it just is not exposed
  yet. Note it is one-to-many: `StaticMesh` -> {`StaticMeshAsset`,`SkinnedMeshAsset`}.
- **Attributes:** `Attribute { const char* key; Variant value; }` - a freeform
  key->Variant, attached via `TypeBuilder::Attribute` (type) / `PropAttribute`
  (property), read via `FindAttribute(prop, key) -> const Attribute*` and
  `FindAttribute(type, key) -> const Variant*` (the two overloads are asymmetric).
  In use today: `displayName`, `category`, `visibleWhen` (`"prop"` or `"prop=1,2"`),
  `range` (`Float4{min,max,step,_}`), `description`. Crucially,
  **`ApplyPropertyPresentation` is ALREADY fully reflection-driven** for label /
  tooltip / conditional visibility - so the attribute-uniformity half of D3 largely
  exists; the gap is the WIDGET selection (leaf types + refs), not presentation.
- **Widgets:** all derive `ui::toolkit::PropertyEditor` (BoolEditor, EnumEditor,
  Float/Float2/Float3/Float4Editor, IntEditor, ColorEditor, RangeEditor,
  StringEditor, and the editor-side `ResourceRefEditor`). A common base to key a
  registry on.
- **Write path:** scalar edits route through `SetComponentPropertyCommand`
  (consecutive same-field edits MERGE into one undo step); ref edits through the
  templated `SetResourceRefCommand<T>` (saves old id, `ref->SetId(new); Rebind`);
  `EntityRef` through `SetEntityRefCommand`. The generic ref path needs a
  TYPE-ERASED ref-set command (SetId + Rebind without `T`) - small, since `Rebind`
  already funnels to `ResourceManager::Bind(TypeInfo&, Guid)`.

## Design

### D1 - a type-erased resource-Ref surface

Add a non-template `RefBase` that `Ref<T>` derives, exposing what the inspector
needs without the static `T`:

```cpp
struct RefBase {                     // in foundation.resource
    Guid id;                                        // the serialized identity
    [[nodiscard]] virtual const TypeInfo* ProductType() const noexcept = 0;
};
template <typename T> class Ref final : public RefBase {
    const TypeInfo* ProductType() const noexcept override { return &T::StaticType(); }
    // id inherited; Proxy<T> + Bind() unchanged
};
```

Then reflection gains two pure queries (populated when a `Ref<T>` is reflected, or
computed by recognizing the `RefBase` sub-object):

- `bool IsResourceRef(const TypeInfo&)`
- `const TypeInfo* ResourceRefProductType(const Instance& refField)` - reads the
  live `RefBase::ProductType()` (or, statically, off the reflected Ref type).

The fourteen identity checks collapse to ONE: "is this property a resource ref?".
The Guid is read/written through `RefBase::id` generically (reflect `id`, or
address it off the `RefBase` sub-instance the same way `WriteBinding` walks nested
addresses). The write also needs a TYPE-ERASED `SetResourceRefCommand` (today it is
templated on `T`): with `RefBase` it becomes `refBase->id = new; refBase->Rebind(...)`
where `Rebind` already dispatches to `ResourceManager::Bind(TypeInfo&, Guid)` - so
the command drops its `<T>` with no new binding machinery.
`EntityRef` stays a SIBLING generic case (`IsEntityRef` -> the entity picker), not
folded into the resource-ref path.

Wire compatibility: `RefBase::id` stays the first serialized member; the
serializer already writes `Ref<T>` by its `id`, so adding a base with the same
field is layout- and wire-neutral (verify with the existing Ref serialization
round-trip test).

### D2 - resource-type -> asset-name(s), derived from the builder registry

A query over the pipeline builder registry:

```cpp
// Pipeline (editor-visible): asset type NAMES whose builder produces this product.
Array<StringView> AssetTypeNamesForProduct(const TypeInfo& productType);
```

Implemented as a new `BuilderRegistry::FindProductAssetNames(const TypeInfo&
productType)` (the reverse of the existing `Find(assetType)`): scan `m_builders`,
match `builder->ProductType() == &productType`, collect each
`builder->AssetType()->name`. One-to-many is expected and correct - `StaticMesh`
yields {`StaticMeshAsset`,`SkinnedMeshAsset`}, and the picker filter is already a
set. No new registration; the product-type link is already on every builder, just
never indexed.

Override hook: an optional `assetType` PROPERTY attribute on the Ref field for the
rare case the registry can't disambiguate (or a resource with no cooker). Reflection
already carries per-property attributes; this is one `FindAttribute` read.

### D3 - the generic property dispatch

Rewrite the per-property builder as an ordered, reflection-driven cascade:

1. `IsResourceRef(prop.type)` -> the ONE generic ref picker (D1 for get/set +
   product type; D2 for the filter). Reuses the existing `ResourceRefEditor`
   widget and the existing property-write undo command.
2. `IsEnum(prop.type)` -> `EnumEditor` (dropdown by name).
3. `IsContainer(prop.type)` -> the list editor.
4. `IsNested(prop)` -> recurse.
5. Leaf value -> a **value-editor registry** keyed by `const TypeInfo*` ->
   `PropertyEditor` factory. Editors register their type(s) at startup
   (explicit-registration composition root + count tripwire, house pattern). The
   hardcoded Float/Float3/Color/Bool/String branches become registrations.

Attributes drive presentation UNIFORMLY across every branch: `range` selects
RangeEditor over FloatEditor, `displayName` sets the label, `description` the
tooltip, `visibleWhen` conditional visibility, `readOnly` disables. Read once,
applied by the cascade - not re-implemented per branch.

Bespoke editors (collision matrix, materials list) stay bespoke but move INTO the
value-editor registry keyed by their type (or by an `editor="collision-matrix"`
attribute), so they too are registrations, not `if` arms.

### D4 - no silent omissions (the anti-regression)

A leaf property that matches no registry entry renders a visible
`"<propName>: unsupported type <typeName>"` row and logs once. A missing widget is
LOUD, not invisible - which structurally kills the recurring bug and makes the
gap self-reporting during development.

## Migration (each phase battery-green, independently landable)

- **P1 - collapse the Ref table.** D1 (`RefBase` + `IsResourceRef` +
  `ResourceRefProductType`) + D2 (builder-registry query) + replace the ~15 ref
  branches (component AND settings) with one generic picker. Delete the
  per-resource `import`s and `Foundation::*.Resource` links that existed only to
  feed the table. Behavior identical: same pickers, same filters. This alone
  closes the bug class.
- **P2 - value-editor registry.** D3 leaf dispatch + uniform attribute handling.
  Migrate every hardcoded value branch to a registered `PropertyEditor` factory;
  fold the bespoke editors in by type/attribute key.
- **P3 - loud-unsupported + settings parity.** D4, and give the settings inspector
  the same generic path (it currently duplicates the ref chain).

## Tests (standing rule: nothing lands without them)

- `IsResourceRef` / `ResourceRefProductType` round-trip for every reflected Ref
  type; `RefBase` serialization round-trip is byte-identical to the old `Ref<T>`.
- `AssetTypeNamesForProduct` returns the right name(s) per product
  (Texture->{TextureAsset}, PropertyAnimationClipResource->
  {PropertyAnimationClipAsset}, ...). **Completeness tripwire:** enumerate every
  component's reflected `resource::Ref<T>` fields and assert each resolves a
  non-empty asset-name set - the test that would have caught all four historical
  omissions at CI time instead of in the user's hands.
- Generic ref picker writes the Guid through the SAME undo command as before and
  `Bind`s a live proxy (headless).
- Value-editor registry: each registered type builds its widget; an UNregistered
  leaf renders the loud-unsupported row (assert the row exists, not that it is
  empty).
- Headless inspector-build over a representative component set: assert EVERY leaf
  property produced a row (no silent gaps).

## Explicitly out of scope

- Rewriting the widgets themselves (the `PropertyEditor` family stays as-is).
- Non-inspector reflection consumers (scripting, MCP) - unaffected; they read the
  same metadata.
- Runtime-side changes beyond `RefBase` (which is a wire-neutral base on an
  existing type).

## Non-obvious constraints (house rules that bite here)

- Reflection registration bodies go in module IMPLEMENTATION units, never
  interface units (gcc gcm-cluster blowup); build gcc early on this track.
- The value-editor registry is an explicit-registration composition root with a
  COUNT tripwire (the `RegisterAll*` + `k*Count` pattern) - discovery is banned.
- Editor-only: no editor concern leaks onto runtime reflected types. `RefBase`
  carries only `id` + the product type (both already runtime-meaningful); the
  asset-name mapping and widgets stay editor/pipeline side.
- Keep `Pipeline` UI-free: `AssetTypeNamesForProduct` lives in a pipeline/editor
  library the inspector links, not in a UI target.
