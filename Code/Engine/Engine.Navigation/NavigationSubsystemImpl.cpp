// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Navigation - implementation unit.
//
// The component reflection (REFLECT_VALUE bodies) lives here, out of the interface units (GCC
// module hygiene). Surfaces the zone + agent components to the editor inspector (displayName/
// category, the zone Ref picker) and the agent's runtime API to script
// (NavAgent.of(entity).navigate(x,y,z) / .stop() / .finished() / .remaining() / .velocity*()).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

module engine.navigation;
import engine.domain;

import foundation.core;
import foundation.runtime;
import foundation.profiler;
import foundation.scene;
import foundation.resource;
import foundation.navigation;
import foundation.navigation.resource;
import foundation.render;
import engine.render;
import foundation.script.facades;

using namespace foundation::core;

namespace engine::navigation
{
    namespace
    {
        // Draw a scene's loaded navmesh (+ agent target lines when enabled) into the debug scene.
        // Reads the LIVE navmesh via NavigationMesh::DebugTriangles, transformed by each zone
        // entity's world matrix - so it shows exactly what agents path on, at the current placement.
        void DrawNavigationDebug(foundation::scene::Scene& scene,
                                 const NavigationSceneSettings& settings,
                                 foundation::render::debug::DebugDraw& draw)
        {
            const Color meshColor{0.20f, 0.85f, 1.0f, 1.0f};
            const Color pathColor{1.0f, 0.85f, 0.20f, 1.0f};

            if (auto* zones = scene.GetSystem<NavMeshZoneComponentManager>())
            {
                zones->ForEach(
                    [&](NavMeshZoneComponent& z, foundation::scene::EntityHandle entity)
                    {
                        foundation::navigation::NavigationZoneResource* product = z.zone.Get();
                        if (product == nullptr || !product->IsValid())
                        {
                            return;
                        }
                        // Rigid (no scale): matches the bake + crowd placement frame, so the
                        // debug overlay shows the navmesh exactly where agents path on it.
                        const Float4x4 world = RigidPart(scene.GetWorldMatrix(entity));
                        Array<Float3> tris;
                        product->mesh.DebugTriangles(tris);
                        for (usize t = 0; t + 2 < tris.Size(); t += 3)
                        {
                            const Float3 a = TransformPoint(tris[t + 0], world);
                            const Float3 b = TransformPoint(tris[t + 1], world);
                            const Float3 c = TransformPoint(tris[t + 2], world);
                            draw.DrawLine(a, b, meshColor);
                            draw.DrawLine(b, c, meshColor);
                            draw.DrawLine(c, a, meshColor);
                        }
                    });
            }

            if (settings.debugDrawBakeStages)
            {
                if (auto* system = scene.GetSystem<NavigationSceneSystem>())
                {
                    const NavigationSceneSystem::BakeStageCache& stages = system->BakeStages();
                    if (!stages.contourLines.IsEmpty() || !stages.walkableSamples.IsEmpty())
                    {
                        // Placed by the zone entity's RIGID frame - the same frame the bake
                        // captured in (zone-local), so stages overlay the mesh exactly.
                        const Float4x4 world =
                            scene.FindEntity(scene.GetEntityId(stages.zoneEntity))
                                    .IsAssigned()
                                ? RigidPart(scene.GetWorldMatrix(stages.zoneEntity))
                                : Float4x4::Identity();
                        const Color contourColor{1.0f, 0.55f, 0.15f, 1.0f}; // orange outlines
                        for (usize i = 0; i + 1 < stages.contourLines.Size(); i += 2)
                        {
                            draw.DrawLine(TransformPoint(stages.contourLines[i], world),
                                          TransformPoint(stages.contourLines[i + 1], world),
                                          contourColor);
                        }
                        // Span-top ticks, strided to a sane draw budget.
                        const Color spanColor{0.35f, 0.9f, 0.35f, 1.0f};
                        const usize stride =
                            1 + stages.walkableSamples.Size() / 20000; // <= ~20k ticks
                        for (usize i = 0; i < stages.walkableSamples.Size(); i += stride)
                        {
                            const Float3 p =
                                TransformPoint(stages.walkableSamples[i], world);
                            draw.DrawLine(p, p + Float3{0, 0.15f, 0}, spanColor);
                        }
                    }
                }
            }

            if (settings.debugDrawPaths)
            {
                if (auto* agents = scene.GetSystem<NavAgentComponentManager>())
                {
                    agents->ForEach(
                        [&](NavAgentComponent& agent, foundation::scene::EntityHandle entity)
                        {
                            const Float3 position = scene.GetWorldPosition(entity);
                            if (agent.hasTarget)
                            {
                                draw.DrawLine(position, agent.target, pathColor);
                            }
                            if (agent.agentId < 0)
                            {
                                return;
                            }
                            // The introspection overlay: crowd state + move-request stage +
                            // live speed intent, floating above the agent - the "why is it
                            // not moving" answer without opening anything.
                            static constexpr StringView kStates[] = {u8"invalid", u8"walking",
                                                                     u8"offmesh"};
                            static constexpr StringView kTargets[] = {
                                u8"none", u8"requesting", u8"valid", u8"velocity", u8"failed"};
                            const StringView state =
                                kStates[agent.crowdState < 3 ? agent.crowdState : 0];
                            const StringView target =
                                kTargets[agent.crowdTargetState < 5 ? agent.crowdTargetState
                                                                    : 0];
                            // One-decimal speed (the formatter has no precision specs).
                            const f32 tenths =
                                static_cast<f32>(
                                    static_cast<i32>(agent.crowdDesiredSpeed * 10.0f)) /
                                10.0f;
                            const String label =
                                Format(u8"{} [{}] {}u/s", state, target, tenths);
                            const Color labelColor =
                                (agent.crowdTargetState == 4) // failed = red at a glance
                                    ? Color{1.0f, 0.35f, 0.30f, 1.0f}
                                    : pathColor;
                            draw.DrawText3D(position + Float3{0, agent.height + 0.3f, 0},
                                            label.AsView(), labelColor);
                        });
                }
            }
        }
    }

    void NavigationSubsystem::Update(f32)
    {
        foundation::runtime::Context* context = GetContext();
        if (context == nullptr)
        {
            return;
        }
        auto* render = context->GetSubsystem<engine::render::RenderSubsystem>();
        if (render == nullptr)
        {
            return;
        }
        for (const SceneEntry& entry : Scenes())
        {
            if (entry.system != nullptr && entry.scene != nullptr &&
                entry.system->Settings().debugDraw)
            {
                PROFILE_SCOPE("Navigation.DebugDraw");
                DrawNavigationDebug(*entry.scene, entry.system->Settings(),
                                    render->DebugScene(*entry.scene));
            }
        }
    }
}

namespace engine::navigation
{
    REFLECT_VALUE(NavMeshZoneComponent, "rtti::engine::navigation")
    {
        builder.Attribute("displayName", String(u8"Nav Mesh Zone"))
            .Attribute("category", String(u8"Navigation"))
            .Attribute("description",
                       String(u8"Marks a box where a navigation mesh is baked, and loads that "
                              u8"baked mesh for agents at run time."))
            .DataVersion(1)
            .Property<&NavMeshZoneComponent::extents>("extents")
            .Property<&NavMeshZoneComponent::cellSize>("cellSize")
            .Property<&NavMeshZoneComponent::cellHeight>("cellHeight")
            .Property<&NavMeshZoneComponent::agentRadius>("agentRadius")
            .Property<&NavMeshZoneComponent::agentHeight>("agentHeight")
            .Property<&NavMeshZoneComponent::agentMaxClimb>("agentMaxClimb")
            .Property<&NavMeshZoneComponent::agentMaxSlopeDegrees>("agentMaxSlopeDegrees")
            .Property<&NavMeshZoneComponent::zone>("zone");
    }

    REFLECT_VALUE(NavAgentComponent, "rtti::engine::navigation")
    {
        builder.Attribute("displayName", String(u8"Nav Agent"))
            .Attribute("category", String(u8"Navigation"))
            .Attribute("description",
                       String(u8"Steers the entity to targets over the navigation mesh, "
                              u8"avoiding other agents."))
            .DataVersion(2) // v2: stopDistance
            .Property<&NavAgentComponent::radius>("radius")
            .Property<&NavAgentComponent::height>("height")
            .Property<&NavAgentComponent::maxSpeed>("maxSpeed")
            .Property<&NavAgentComponent::maxAcceleration>("maxAcceleration")
            .Property<&NavAgentComponent::stopDistance>("stopDistance")
            .Property<&NavAgentComponent::moveEntity>("moveEntity")
            // Runtime API (Track A): NavAgent.of(entity) -> a re-resolving handle.
            .Method<&foundation::script::ComponentOf<NavAgentComponent>, NavAgentComponent>("of")
            .Method<&NavAgentComponent::navigate>("navigate", {"x", "y", "z"})
            .Method<&NavAgentComponent::navigateAt>("navigateAt",
                                                    {"x", "y", "z", "speed", "stopDistance"})
            .Method<&NavAgentComponent::setSpeed>("setSpeed", {"speed"})
            .Method<&NavAgentComponent::setStopDistance>("setStopDistance", {"distance"})
            .Method<&NavAgentComponent::speed>("speed")
            .Method<&NavAgentComponent::state>("state")
            .Method<&NavAgentComponent::targetState>("targetState")
            .Method<&NavAgentComponent::desiredSpeed>("desiredSpeed")
            .Method<&NavAgentComponent::corners>("corners")
            .Method<&NavAgentComponent::stop>("stop")
            .Method<&NavAgentComponent::finishedNav>("finished")
            .Method<&NavAgentComponent::remaining>("remaining")
            .Method<&NavAgentComponent::velocityX>("velocityX")
            .Method<&NavAgentComponent::velocityY>("velocityY")
            .Method<&NavAgentComponent::velocityZ>("velocityZ");
    }

    REFLECT_VALUE(NavigationSceneSettings, "rtti::engine::navigation")
    {
        builder.DataVersion(2); // v2: debugDrawBakeStages
        builder.Property<&NavigationSceneSettings::debugDraw>("debugDraw");
        builder.Property<&NavigationSceneSettings::debugDrawPaths>("debugDrawPaths");
        builder.Property<&NavigationSceneSettings::debugDrawBakeStages>("debugDrawBakeStages");
    }

    void RegisterNavigationComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_NavMeshZoneComponent();
            RttiRegisterValue_NavAgentComponent();
            RttiRegisterValue_NavigationSceneSettings();
            return true;
        }();
        (void)once;
    }

    void RegisterNavigationScriptFacade()
    {
        RegisterNavigationComponentReflection(); // component TypeData (incl `of`) built first

        // Surface NavAgentComponent to script (NavAgent.of(entity).navigate(...)): register it,
        // seed the emission root (reachability), and make the class name prelude-visible.
        GlobalTypeRegistry().Register(TypeOf<NavAgentComponent>());
        foundation::script::RegisterExtraScriptRootType(&TypeOf<NavAgentComponent>());
        foundation::script::RegisterExtraFacadeName(u8"NavAgentComponent");
    }
}

namespace engine::navigation
{
    const engine::DomainModule& NavigationDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::navigation::kNavigationResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"navigation",
            .installScene = &AddNavigationSceneManagers,
            .registerReflection = &RegisterNavigationComponentReflection,
            .registerScriptFacade = &RegisterNavigationScriptFacade,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
