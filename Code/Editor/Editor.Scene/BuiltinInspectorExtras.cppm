// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :builtin_inspector_extras partition.
//
// The inspector's extras for the engine's own components and settings blocks
// (Documentation/Specs/editor-gui.md, 5.3, layer 4, parts 3 and 4), registered by type into the
// inspector's registry by RegisterSceneEditor:
//   - a rigid body whose shape is Cooked with no collision shape set has no collider: said under
//     its header while it is so (it follows an edit, not only a rebuild);
//   - a script component's behaviours, a source of their own (ScriptBehaviorsSource);
//   - a navigation zone's Bake: the in-zone static geometry baked and written into the zone's
//     asset, the outcome said. It runs at once on the main thread, as the bake's interface is one
//     call; a bake as a job waits for that call to be split (collect here, bake on a worker);
//   - the physics block's collision groups and the scene script block's level properties, sources
//     of their own (SettingsExtraSources).

module;
#include "Core/Prelude.h"

export module editor.scene:builtin_inspector_extras;

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.physics;
import engine.physics;
import engine.navigation;
import engine.animation; // PropertyAnimatorComponent (its Edit Clip)
import editor.core;
import editor.navigation;
import :edit;
import :inspector_extras;
import :scene_page_interface; // the animation panel an animator's clip opens in
import :script_behaviors_source;
import :settings_extra_sources;
import engine.script;

using namespace foundation::core;

export namespace editor
{
    /// BuiltinInspectorExtras::kNoColliderText, at namespace scope for another library to read (MSVC
    /// exports no class's static constexpr member from a shared library).
    inline constexpr StringView kBuiltinInspectorExtrasNoColliderText =
        u8"Shape is Cooked but no collision shape is set - this body has no collider. Assign one (import a mesh with Generate "
        u8"collision, or create a Collision Shape asset).";

    // A struct of static functions rather than a function: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct BuiltinInspectorExtras
    {
        static constexpr StringView kNoColliderText = kBuiltinInspectorExtrasNoColliderText;

        static void Register(InspectorExtrasRegistry& registry)
        {
            registry.Register(TypeOf<engine::physics::RigidBodyComponent>(), [](InspectorExtras& extras) { AddNoCollider(extras); });
            registry.Register(TypeOf<engine::navigation::NavMeshZoneComponent>(), [](InspectorExtras& extras) { AddBake(extras); });
            registry.Register(TypeOf<engine::script::ScriptComponent>(), [](InspectorExtras& extras) { AddBehaviors(extras); });
            registry.Register(TypeOf<engine::physics::PhysicsSceneSettings>(), [](InspectorExtras& extras) { AddCollisionGroups(extras); });
            registry.Register(TypeOf<engine::script::SceneScriptSettings>(), [](InspectorExtras& extras) { AddLevelProperties(extras); });
            registry.Register(TypeOf<engine::animation::PropertyAnimatorComponent>(), [](InspectorExtras& extras) { AddEditClip(extras); });
        }

    private:
        // A script component's behaviours: a source of their own (reflection does not show them).
        static void AddBehaviors(InspectorExtras& extras)
        {
            auto source = MakeUnique<ScriptBehaviorsSource>(EditorRootAllocator(), extras.Edit(), extras.Context(), extras.Entities());
            ScriptBehaviorsSource* raw = source.Get();
            extras.AddSection(Move(source), [raw]() { raw->Refresh(); });
        }
        // The physics block's collision groups (reflection does not show them).
        static void AddCollisionGroups(InspectorExtras& extras)
        {
            auto source = MakeUnique<CollisionGroupsSource>(EditorRootAllocator(), extras.Edit());
            CollisionGroupsSource* raw = source.Get();
            extras.AddSection(Move(source), [raw]() { raw->Refresh(); });
        }
        // The scene script block's Level class's properties (its overrides are not reflected).
        static void AddLevelProperties(InspectorExtras& extras)
        {
            auto source = MakeUnique<LevelScriptSource>(EditorRootAllocator(), extras.Edit(), extras.Context());
            LevelScriptSource* raw = source.Get();
            extras.AddSection(Move(source), [raw]() { raw->Refresh(); });
        }
        // An animator's clip in the page's animation panel, animating its entity (no clip: the panel
        // bound to it, to make one).
        static void AddEditClip(InspectorExtras& extras)
        {
            InspectorExtras* at = &extras;
            extras.AddAction(u8"Animation", u8"Edit Clip", u8"Open the clip in the animation panel under the viewport, animating this entity",
                             [at]()
                             {
                                 EditorContext* context = at->Context();
                                 ISceneEditorPage* page = context != nullptr ? ServiceOf<ISceneEditorPage>(context->ActivePage()) : nullptr;
                                 if (page == nullptr || at->Entities().IsEmpty())
                                     return;
                                 SceneEditContext& edit = at->Edit();
                                 const Guid entity = at->Entities()[0];
                                 auto* animators = edit.Scene().GetSystem<engine::animation::PropertyAnimatorComponentManager>();
                                 const foundation::scene::EntityHandle handle = edit.Resolve(entity);
                                 engine::animation::PropertyAnimatorComponent* animator = animators != nullptr && handle.IsAssigned() ? animators->Get(handle) : nullptr;
                                 page->EditAnimationClip(animator != nullptr ? animator->clip.id : Guid{}, entity);
                             });
        }

        static void AddNoCollider(InspectorExtras& extras)
        {
            InspectorExtras* at = &extras;
            extras.AddNotice(u8"Collision", [at]() -> String
                             {
                                 SceneEditContext& edit = at->Edit();
                                 auto* bodies = edit.Scene().GetSystem<engine::physics::RigidBodyComponentManager>();
                                 for (const Guid& id : at->Entities())
                                 {
                                     const foundation::scene::EntityHandle e = edit.Resolve(id);
                                     engine::physics::RigidBodyComponent* body = bodies != nullptr && e.IsAssigned() ? bodies->Get(e) : nullptr;
                                     if (body != nullptr && body->shape == foundation::physics::ShapeKind::Cooked && body->collisionShape.id.IsNil())
                                         return String(kNoColliderText);
                                 }
                                 return String{};
                             });
        }

        static void AddBake(InspectorExtras& extras)
        {
            InspectorExtras* at = &extras;
            extras.AddAction(u8"Navigation", u8"Bake Navigation",
                             u8"Bake the zone's static geometry (static bodies, terrain) into its Navigation Zone asset; cook to apply it.",
                             [at]()
                             {
                                 EditorContext* context = at->Context();
                                 SceneEditContext& edit = at->Edit();
                                 if (context == nullptr)
                                     return;
                                 if (context->Project() == nullptr)
                                 {
                                     context->Notify(NoticeKind::Error, u8"No project is open.");
                                     return;
                                 }
                                 auto* zones = edit.Scene().GetSystem<engine::navigation::NavMeshZoneComponentManager>();
                                 for (const Guid& id : at->Entities())
                                 {
                                     const foundation::scene::EntityHandle entity = edit.Resolve(id);
                                     engine::navigation::NavMeshZoneComponent* zone = zones != nullptr && entity.IsAssigned() ? zones->Get(entity) : nullptr;
                                     if (zone == nullptr)
                                         continue;
                                     foundation::content::Instance* target = zone->zone.id.IsNil() ? nullptr : context->Project()->SourceDb().GetInstance(zone->zone.id);
                                     if (target == nullptr)
                                     {
                                         context->Notify(NoticeKind::Warning, u8"Assign a Navigation Zone asset to this zone before baking.");
                                         continue;
                                     }
                                     const navigation::BakeResult result =
                                         navigation::BakeNavigationZone(edit.Scene(), entity, *target, navigation::ParallelBakeEnabled(*context));
                                     context->Notify(result.baked ? NoticeKind::Success : NoticeKind::Warning, navigation::DescribeBake(result).AsView());
                                 }
                             });
        }
    };
}
