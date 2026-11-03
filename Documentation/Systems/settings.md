# Settings

> Status: CURRENT
> Verified: 2026-08-12 @ 1abdb98d
> Track: [[settings-and-export-track]]

`foundation.settings::Settings` is the typed, versioned, backend-agnostic settings primitive: a store
of reflected `ISerializable` sections persisted through the one `ISerializer` stack. Shipped and used
engine-wide (editor prefs, project manifest, per-scene subsystem settings). The applied multi-store
layering + the project manager are in `Documentation/Systems/project-and-settings.md`.

## The store (`foundation.settings::Settings`)

A settings section is a reflected `ISerializable` struct with a data version; a subsystem declares one
(`RTTI_DEFINE_OBJECT_VERSIONED(T, domain, version)`), and the store keys sections by the reflected type
name - no path strings:
- `settings.Section<T>()` - lazily created/loaded typed accessor.
- `settings.MarkChanged<T>()` - fires `OnChanged(typeName)` (the store cannot detect field writes, so
  callers announce).
- `settings.OnChanged(Function<void(StringView)>)` - subscribers react (theme changed -> re-theme).
- `Load(IStream, SerializerFactory, TypeRegistry)` / `Save(IStream, SerializerFactory)` - backend
  chosen by the caller: `XmlSerializerFactory` for hand-editable files, a binary factory in tests. Not
  tied to XML.

Reads take the whole section (no field-merge); a field missing from a stored section keeps the struct's
member-initializer default, so adding fields later is backward-safe.

## Registration + unknown-section passthrough

Sections must be REGISTERED (type registry + serializable registry) before a store loads - an
unregistered section type fails the whole `Settings::Load` (so per-module sections register in that
module's editor entry point before the store loads). Unknown-section passthrough SHIPPED: a section whose
type is unknown to THIS build is captured (`m_unknownSections`, `UnknownSectionCount()`) and re-emitted
verbatim inside its frame on the next `Save`, so a newer build's extra settings survive an older build's
open + re-save. This works on a self-describing backend (XML: framed sections a reader can skip); on a
positional binary backend an unknown-shape section cannot be skipped, so the load aborts there (settings
files are XML, so passthrough holds).

## Layering

The designed single-store Default/User/Project LAYER STACK did NOT ship as an in-store construct; there
is no `SettingsLayer` enum. Layering is achieved by SEPARATE `Settings` store INSTANCES, one per file
(a per-user store + a per-project store + the project manifest), each a flat store. The precedence and
the three concrete stores are documented in `project-and-settings.md`.

## Consumers

Many reflected sections ship through this machinery: editor prefs (`EditorExportSettings`,
`RecentProjectsSettings`, `EditorDockLayoutSettings`, ...), the project manifest (`ProjectSettings`),
and per-scene/subsystem settings (`PhysicsSceneSettings`, `AudioBusSettings`, `PostProcessSettings`,
`FogSettings`, `ViewSettings`, `WindowSettings`, ...). The user-data-dir helper (for the per-user file)
lives in `foundation.core` (`System.cppm`).

A game's save is one more: `SaveValues` (`SaveValues.cppm`), the open-ended section. Its fields are not
known ahead, so it keeps typed values (bool, int, float, text) by key, sorted, with kinds written by
name. A save file is an ordinary settings file holding that one section; the run that owns it, the
`Save` script facade and where each host keeps the file are in `game-instance.md` and
`Documentation/Archive/save-data-history.md`.

## Deferred

The dynamic **`PropertyBag` + `SettingsKey<T>`** bag (a compact tagged-`Value` map for genuinely
open-ended / plugin key-values) is NOT built. The one consumer with open-ended keys so far, a game's
save, has its own section (`SaveValues`, above), which is what this would have layered in as.

---

The Traktor `Core/Settings` survey, the original one-store-with-a-layer-stack design (superseded by the
per-file-store model), and the phased plan are in `Documentation/Archive/settings-design-history.md`.
