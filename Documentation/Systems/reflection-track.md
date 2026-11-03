# Reflection - the reflected surface for scripting + tooling

> Status: CURRENT
> Verified: 2026-08-12 @ f090a071
> Track: [[reflection-track]]

An ongoing coverage effort: reflect the useful surface of the codebase so it is usable for scripting and
tooling without hand-written bindings or serialize-scan fallbacks. The facilities + the mechanism
additions (nested members, computed properties, container primitives, collections-in-scripts, the
generic list editor) shipped; coverage grows consumer-by-consumer.

## Facilities (`foundation.core` `:reflection`)

Macros (the `Core/Prelude.h` family, renamed from the old `DRACONIC_*`): `REFLECT_MEMBERS` (intrusive +
properties, via the `TypeBuilder`), `REFLECT_VALUE` (value types + properties), `REFLECT_ENUM`, and
`RTTI_DEFINE_OBJECT` / `RTTI_DEFINE_OBJECT_VERSIONED` (identity, with the versioned form carrying a data
version). Property attributes drive tooling: `displayName`, `range` (sliders), `description` (tooltips),
`visibleWhen` (conditional rows), `category`.

- **Enums** reflect their value names, so inspectors render dropdowns by NAME (not raw int rows) and
  scripts see named values.
- **Containers** reflect through `ContainerInfo` (`TypeInfo.container`): `Array<T>`, C-arrays exposed as
  a flat row-major view (e.g. matrices as `f32[N][N]`), and the homogeneous `Array<UniquePtr<T>>` poly
  container (`RegisterUniquePtrArrayType`). Structural mutation is guarded by a reflection mutation
  generation (so a `Variant` borrow into a container is invalidated on structural change, never a stale
  address).
- **Nested + computed** members: a property can be a nested reflected value (the generic page recurses
  into it) or a computed getter (see the mechanism additions below).

## Consumers that light up as coverage grows

1. **Generic asset page** - reflection-FIRST (serialize-scan stays as the fallback for still-unreflected
   types): real labels, sliders, tooltips, conditional rows, and enum dropdowns by name.
2. **Script backends** (AngelScript + Luau) - both harvest reflected types, so every newly reflected
   type/enum becomes scriptable surface with no hand bindings.
3. **Bespoke inspectors** (particle modules, graph data) - reflected instead of hand-built `Cast<T>`
   dispatches.
4. **The generic list editor** (`Editor.Scene` `InspectorView`) - reflected containers get add/remove/
   reorder rows, and (2026-09-21) a struct element gets a per-slot expander of its leaf rows edited
   through a `ComponentPropertyPath` (editor.md 3.5).

## Coverage phases

Consumer-driven, each paying off on landing:
- **P1 - Editor assets + their enums**: CLOSED (the flat asset types + their enums reflected, the
  pattern-setter being `TextureAsset`; the base `Asset::fileName` reflected as its `SourcePath`).
- **P2 - Particle modules + animation graph/emitter data**: DONE (the particle module types reflected,
  full effect traversal wired).
- **P3 - Runtime gameplay surface for scripting**: audio/physics knobs and gameplay descs, as consumers
  demand.
- **P4 - Consumer upgrades**: the generic page reflection-first mode + enum dropdowns.

## Mechanism additions (shipped)

The sweep drove several Core reflection additions (each unblocking a coverage wave):
- **Nested reflected members** - a by-value nested reflected struct as a property the generic page
  recurses into (the resolution of the nested-non-copyable-member blocker).
- **`ComputedProperty`** - a getter-backed property (distinct from the Nested kind).
- **Two container primitives** - the C-array flat view + the `Array<UniquePtr<T>>` poly container, added
  for the particle-module sweep.
- **Collections-in-scripts** - reflected container MEMBERS bind as script ops on the owner
  (`NAME_count` / `NAME_at(i)` / ...), constructor-less reflected types bind as returned HANDLES, and a
  `Variant` BORROW mode carries nested-value / non-Object element handles (both backends, generation-
  guarded).
- **The generic list editor** in the inspector.

## Non-goals

Not everything is reflected - identity-only `RTTI_DEFINE_OBJECT` stays for types with no tooling/script
consumer. Coverage is demand-driven, not exhaustive.

---

The working-log detail (the measured-gap census, the nested-member blocker + Fable's decision, the
container-primitives blocker, and the unit-by-unit collections-in-scripts progress) is in
`Documentation/Archive/reflection-track-history.md`.
