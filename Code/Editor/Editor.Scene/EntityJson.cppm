// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - :entity_json partition.
//
// An entity as the agent tools read it, through reflection: its identity, hierarchy, transform
// and every component with its properties (references as guids, enums by name, nested structures
// and lists expanded). entity_inspect answers with it for a scene page and for a running game;
// pie_run reads field paths through it. Shared so the two never spell a value differently.
module;
#include "Core/Prelude.h"

export module editor.scene:entity_json;

import foundation.core;
import foundation.json;
import foundation.scene;
import foundation.resource;

using namespace foundation::core;

export namespace editor
{
    [[nodiscard]] foundation::json::JsonValue GuidJson(const Guid& id);
    [[nodiscard]] foundation::json::JsonValue Float3Json(const Float3& v);
    [[nodiscard]] foundation::json::JsonValue QuadJson(f32 a, f32 b, f32 c, f32 d);

    /// A leaf value as JSON: what the Variant's type says it is. A reference-shaped value (a
    /// resource ref) is its id, an enum its enumerator's name, an entity ref its guid.
    [[nodiscard]] foundation::json::JsonValue ValueJson(const Variant& value);
    /// One property: a leaf through its Variant; a nested structure recursed into; a container
    /// as an array of its elements.
    [[nodiscard]] foundation::json::JsonValue PropertyJson(const PropertyInfo& property,
                                                           const Instance& instance);
    /// A reflected structure's properties, by name, in declaration order.
    [[nodiscard]] foundation::json::JsonValue PropertiesJson(const TypeInfo& type,
                                                             const Instance& instance);

    /// The entity as the agent sees it: identity, hierarchy, transform, and every component the
    /// scene holds for it with its reflected properties; a Script component also lists its
    /// behaviours, each override named from its cooked class (bound through `resources` when the
    /// behaviour holds none; by hash, #n, when neither has it).
    [[nodiscard]] foundation::json::JsonValue
    EntityJson(foundation::scene::Scene& scene, foundation::scene::EntityHandle handle,
               foundation::resource::ResourceManager* resources = nullptr);

    /// The manager holding `component` for the entity: by serialization id ("light",
    /// "physics.RigidBody") or by the reflected type's name ("LightComponent"); null when none.
    [[nodiscard]] foundation::scene::ComponentManagerBase*
    FindComponentManager(foundation::scene::Scene& scene, foundation::scene::EntityHandle entity,
                         StringView component);

    /// The field of a running behaviour of `className` on the entity (its instance's member, a
    /// private one too); false when the entity runs no such behaviour or its class no such field.
    [[nodiscard]] bool BehaviorField(foundation::scene::Scene& scene,
                                     foundation::scene::EntityHandle handle, StringView className,
                                     StringView field, foundation::json::JsonValue& out);

    /// An entity by guid, slash path or name; unassigned when none.
    [[nodiscard]] foundation::scene::EntityHandle FindEntity(foundation::scene::Scene& scene,
                                                             StringView text);

    /// A field path's value: `worldPosition`, `position`, `rotation`, `scale`, `active`,
    /// `<component>.<property>` (the component as entity_inspect names it, which may hold dots), or
    /// `<BehaviorClass>.<field>` of a behaviour running on it, followed by `.x`/`.y`/`.z`/`.w`, a
    /// key or an index to go inside. When the path reads nothing, the error is the reason, naming
    /// `entityText`.
    [[nodiscard]] Result<foundation::json::JsonValue, String>
    EntityFieldJson(foundation::scene::Scene& scene, foundation::scene::EntityHandle handle,
                    StringView entityText, StringView path);
}
