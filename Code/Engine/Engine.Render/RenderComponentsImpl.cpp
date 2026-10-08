// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Render - render.subsystem implementation unit: component reflection bodies.
//
// Kept OUT of the :components interface partition (REFLECT_* bodies make GCC emit a
// gcm cluster; see gcc-module-interface-hygiene). RenderComponents.cppm declares
// RegisterRenderComponentReflection(); this unit defines it + the RttiRegister* bodies.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.render;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.rhi;
import foundation.render;          // debug::DebugDraw (the DebugDraw facade wraps DebugScene's accumulator)
import foundation.script;          // IScriptContext::GetService + CurrentScriptContext (the render service)
import foundation.script.facades; // ComponentOf<T> + RegisterExtra* (the script `.of` surface, Track A)
import :subsystem;                 // RenderSubsystem + RenderScriptBinding (the DebugDraw facade target)

using namespace foundation::core;
using namespace foundation::render;
namespace core = foundation::core;

// ============================================================================================
// Reflection (tooling: the editor inspector auto-generates property grids from these).
// Pointer/RefPtr/array fields (mesh, material, textures, bone matrices) are deliberately not
// reflected - they need resource-picker editors. NON-export namespace:
// the macros expand static helpers (internal linkage), per the CoreReflection.cppm pattern.
// ============================================================================================
namespace engine::render
{

    REFLECT_ENUM(LightType, "rtti::engine::render")
    {
        builder.Value("Directional", LightType::Directional);
        builder.Value("Point", LightType::Point);
        builder.Value("Spot", LightType::Spot);
    }

    REFLECT_ENUM(CameraProjection, "rtti::engine::render")
    {
        builder.Value("Perspective", CameraProjection::Perspective);
        builder.Value("Orthographic", CameraProjection::Orthographic);
    }

    REFLECT_ENUM(ShadowUpdateMode, "rtti::engine::render")
    {
        builder.Value("Realtime", ShadowUpdateMode::Realtime);
        builder.Value("Static", ShadowUpdateMode::Static);
    }

    REFLECT_ENUM(ProbeUpdateMode, "rtti::engine::render")
    {
        builder.Value("Static", ProbeUpdateMode::Static);
        builder.Value("Realtime", ProbeUpdateMode::Realtime);
        builder.Value("Manual", ProbeUpdateMode::Manual);
    }

    REFLECT_VALUE(MeshComponent, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Mesh"))
            .Attribute("category", String(u8"Rendering"))
            .DataVersion(5) // v5: fade (v4: LOD knobs, v3: unified materials array)
            // Script (Track A): MeshComponent.of(entity) -> a re-resolving handle; `color`/`visible`
            // set live from a behavior. `mesh`/`materials` are resource refs - swapped via the
            // resource-resolve primitive (Phase 1b), not this raw property.
            .Method<&foundation::script::ComponentOf<MeshComponent>, MeshComponent>("of")
            .Property<&MeshComponent::mesh>("mesh")
            .Property<&MeshComponent::color>("color")
            .Property<&MeshComponent::visible>("visible")
            .Property<&MeshComponent::fade>("fade")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("description",
                           String(u8"Fades the mesh out with a dither, 0 = solid, 1 = gone (a "
                                  u8"cutaway). Its shadow stays whole."))
            .Property<&MeshComponent::lodBias>("lodBias")
            .PropAttribute("displayName", String(u8"LOD Bias"))
            .PropAttribute("description",
                           String(u8"Positive selects coarser LODs sooner (each unit halves the "
                                  u8"effective screen coverage); negative holds detail longer."))
            .Property<&MeshComponent::forceLod>("forceLod")
            .PropAttribute("displayName", String(u8"Force LOD"))
            .PropAttribute("description",
                           String(u8"Pin one LOD level (0 = finest). -1 = automatic selection."))
            // The material slots as a reflected container - the generic list editor renders it. The
            // description surfaces as the list's hover tooltip (the slot-0 / submesh semantics).
            .Nested<&MeshComponent::materials>("materials")
            .PropAttribute("description",
                           String(u8"Material slots, indexed by the mesh's submesh material index. "
                                  u8"Slot 0 also covers single-material meshes and any submesh whose "
                                  u8"index has no slot."));
    }

    REFLECT_VALUE(InstancedMeshComponent, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Instanced Mesh"))
            .Attribute("category", String(u8"Rendering"))
            .Method<&foundation::script::ComponentOf<InstancedMeshComponent>, InstancedMeshComponent>(
                "of")
            .Property<&InstancedMeshComponent::mesh>("mesh")
            .Property<&InstancedMeshComponent::material>("material")
            .Property<&InstancedMeshComponent::color>("color")
            .Property<&InstancedMeshComponent::visible>("visible");
    }

    REFLECT_VALUE(CameraComponent, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Camera"))
            .Attribute("category", String(u8"Rendering"))
            .DataVersion(2) // v1: projection + orthoHeight; v2: target + targetInterval
            .Method<&foundation::script::ComponentOf<CameraComponent>, CameraComponent>("of")
            // The mode first: the inspector shows only the fields of the chosen projection.
            .Property<&CameraComponent::projection>("projection")
            .Property<&CameraComponent::fovYRadians>("fovYRadians")
            .PropAttribute("displayName", String(u8"Field Of View"))
            .PropAttribute("description", String(u8"Vertical field of view (radians)"))
            .PropAttribute("range", Float4{0.10f, 3.04f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"projection=0")) // Perspective only
            .Property<&CameraComponent::aspect>("aspect")
            .Property<&CameraComponent::nearZ>("nearZ")
            .Property<&CameraComponent::farZ>("farZ")
            .Property<&CameraComponent::clearColor>("clearColor")
            .Property<&CameraComponent::primary>("primary")
            .Property<&CameraComponent::orthoHeight>("orthoHeight")
            .PropAttribute("description",
                           String(u8"Orthographic: the world-space height the view spans"))
            .PropAttribute("visibleWhen", String(u8"projection=1")) // Orthographic only
            .Property<&CameraComponent::target>("target")
            .PropAttribute("description",
                           String(u8"A render texture to draw into instead of the screen"))
            .Property<&CameraComponent::targetInterval>("targetInterval")
            .PropAttribute("description",
                           String(u8"Render the target every Nth frame (1 = every frame)"))
            .PropAttribute("range", Float4{1.0f, 60.0f, 1.0f, 0.0f});
    }

    REFLECT_VALUE(LightComponent, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Light"))
            .Attribute("category", String(u8"Rendering"))
            // Script (Track A): LightComponent.of(entity) -> live color/intensity/range/enabled/etc.
            .Method<&foundation::script::ComponentOf<LightComponent>, LightComponent>("of")
            .DataVersion(1) // v1: shadowDepthBiasScale, shadowNormalBias, shadowStrength
            .Property<&LightComponent::type>("type")
            .Property<&LightComponent::color>("color")
            .Property<&LightComponent::intensity>("intensity")
            .PropAttribute("range", Float4{0.0f, 50.0f, 0.1f, 0.0f})
            .Property<&LightComponent::range>("range")
            .PropAttribute("range", Float4{0.0f, 500.0f, 0.5f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"type=1,2"))
            .PropAttribute("description", String(u8"Falloff distance (point/spot lights)"))
            .Property<&LightComponent::innerAngle>("innerAngle")
            .PropAttribute("range", Float4{0.0f, 1.55f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"type=2"))
            .PropAttribute("description", String(u8"Spot cone inner half-angle (radians)"))
            .Property<&LightComponent::outerAngle>("outerAngle")
            .PropAttribute("range", Float4{0.0f, 1.55f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"type=2"))
            .PropAttribute("description", String(u8"Spot cone outer half-angle (radians)"))
            .Property<&LightComponent::shadowUpdate>("shadowUpdate")
            .PropAttribute("visibleWhen", String(u8"castsShadows"))
            .Property<&LightComponent::enabled>("enabled")
            .Property<&LightComponent::castsShadows>("castsShadows")
            .Property<&LightComponent::shadowStrength>("shadowStrength")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"castsShadows"))
            .PropAttribute("description", String(u8"How dark the shadow gets: 1 = full, 0 = none"))
            .Property<&LightComponent::shadowNormalBias>("shadowNormalBias")
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"castsShadows"))
            .PropAttribute("description",
                           String(u8"Normal offset in shadow texels: raise it if a surface the light "
                                  u8"grazes shadows itself (acne), lower it if a shadow parts from its "
                                  u8"caster"))
            .Property<&LightComponent::shadowDepthBiasScale>("shadowDepthBiasScale")
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"castsShadows"))
            .PropAttribute("description",
                           String(u8"Depth-compare bias, as a scale of the default for the light's "
                                  u8"type (1 = default)"));
    }

    // The scene-bound render handle: SceneRender.of(scene).setMesh(entity, id) / setMaterial(...).
    // `of` returns SceneRender by value (concrete cross-backend return, no ReturnType-override), like
    // ScenePhysics. setMesh/setMaterial are world ops keyed by entity that swap a resource::Ref by id.
    REFLECT_VALUE(SceneRender, "rtti::engine::render")
    {
        builder.Method<&SceneRender::setMesh>("setMesh", {"entity", "resourceId"});
        builder.Method<static_cast<bool (SceneRender::*)(foundation::script::Entity, Guid) const>(
            &SceneRender::setMaterial)>("setMaterial", {"entity", "resourceId"});
        builder.Method<static_cast<bool (SceneRender::*)(foundation::script::Entity, Guid, i32) const>(
            &SceneRender::setMaterial)>("setMaterial", {"entity", "resourceId", "slot"});
        builder.Method<&SceneRender::setCameraTarget>("setCameraTarget", {"entity", "textureId"});
        builder.Method<static_cast<Float3 (SceneRender::*)(Float3) const>(&SceneRender::lightAt)>(
            "lightAt", {"position"});
        builder.Method<static_cast<Float3 (SceneRender::*)(Float3, u32) const>(&SceneRender::lightAt)>(
            "lightAt", {"position", "groupMask"});
        builder.Method<&SceneRender::of>("of", {"scene"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    // --- DebugDraw facade bodies (the impl unit sees :subsystem's RenderSubsystem) ---
    namespace
    {
        // The scene's per-scene debug accumulator, via the per-context render service (null-safe).
        [[nodiscard]] debug::DebugDraw* SceneDebug(foundation::scene::Scene* scene)
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            foundation::script::IScriptContext* ctx = foundation::script::CurrentScriptContext();
            RenderScriptBinding* binding =
                ctx != nullptr ? static_cast<RenderScriptBinding*>(ctx->GetService(kRenderScriptService))
                               : nullptr;
            return (binding != nullptr && binding->render != nullptr)
                       ? &binding->render->DebugScene(*scene)
                       : nullptr;
        }
    }

    void DebugDraw::line(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, f32 r, f32 g, f32 b) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawLine(Float3(x0, y0, z0), Float3(x1, y1, z1), Color(r, g, b, 1.0f));
        }
    }
    void DebugDraw::ray(f32 x, f32 y, f32 z, f32 dx, f32 dy, f32 dz, f32 r, f32 g, f32 b) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawRay(Float3(x, y, z), Float3(dx, dy, dz), Color(r, g, b, 1.0f));
        }
    }
    void DebugDraw::arrow(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, f32 r, f32 g, f32 b) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawArrow(Float3(x0, y0, z0), Float3(x1, y1, z1), Color(r, g, b, 1.0f));
        }
    }
    void DebugDraw::sphere(f32 x, f32 y, f32 z, f32 radius, f32 r, f32 g, f32 b) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawWireSphere(Float3(x, y, z), radius, Color(r, g, b, 1.0f));
        }
    }
    void DebugDraw::cross(f32 x, f32 y, f32 z, f32 size, f32 r, f32 g, f32 b) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawCross(Float3(x, y, z), size, Color(r, g, b, 1.0f));
        }
    }
    void DebugDraw::text(f32 x, f32 y, f32 z, String label, f32 r, f32 g, f32 b) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawText3D(Float3(x, y, z), label.AsView(), Color(r, g, b, 1.0f));
        }
    }

    void DebugDraw::line(Float3 from, Float3 to, Color color) const { line(from, to, color, false); }
    void DebugDraw::line(Float3 from, Float3 to, Color color, bool overlay) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawLine(from, to, color, overlay);
        }
    }
    void DebugDraw::ray(Float3 origin, Float3 direction, Color color) const
    {
        ray(origin, direction, color, false);
    }
    void DebugDraw::ray(Float3 origin, Float3 direction, Color color, bool overlay) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawRay(origin, direction, color, overlay);
        }
    }
    void DebugDraw::wireBox(Float3 min, Float3 max, Color color) const { wireBox(min, max, color, false); }
    void DebugDraw::wireBox(Float3 min, Float3 max, Color color, bool overlay) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawWireBox(min, max, color, overlay);
        }
    }
    void DebugDraw::wireSphere(Float3 center, f32 radius, Color color) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawWireSphere(center, radius, color);
        }
    }
    void DebugDraw::cross(Float3 center, f32 size, Color color) const { cross(center, size, color, false); }
    void DebugDraw::cross(Float3 center, f32 size, Color color, bool overlay) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawCross(center, size, color, overlay);
        }
    }
    void DebugDraw::arrow(Float3 start, Float3 end, Color color) const { arrow(start, end, color, 0.1f); }
    void DebugDraw::arrow(Float3 start, Float3 end, Color color, f32 headSize) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawArrow(start, end, color, headSize);
        }
    }
    void DebugDraw::text(Float3 worldPosition, String label, Color color) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawText3D(worldPosition, label.AsView(), color);
        }
    }
    void DebugDraw::screenText(f32 x, f32 y, String label, Color color) const
    {
        screenText(x, y, Move(label), color, 1.0f);
    }
    void DebugDraw::screenText(f32 x, f32 y, String label, Color color, f32 scale) const
    {
        if (debug::DebugDraw* d = SceneDebug(scene))
        {
            d->DrawScreenText(x, y, label.AsView(), color, scale);
        }
    }

    REFLECT_VALUE(DebugDraw, "rtti::engine::render")
    {
        using Numbers9 = void (DebugDraw::*)(f32, f32, f32, f32, f32, f32, f32, f32, f32) const;
        using Numbers7 = void (DebugDraw::*)(f32, f32, f32, f32, f32, f32, f32) const;
        using Segment = void (DebugDraw::*)(Float3, Float3, Color) const;
        using SegmentOverlay = void (DebugDraw::*)(Float3, Float3, Color, bool) const;
        using Marker = void (DebugDraw::*)(Float3, f32, Color) const;
        using MarkerOverlay = void (DebugDraw::*)(Float3, f32, Color, bool) const;
        // Ours by the numbers (r, g, b; depth tested).
        builder.Method<static_cast<Numbers9>(&DebugDraw::line)>(
            "line", {"x0", "y0", "z0", "x1", "y1", "z1", "r", "g", "b"});
        builder.Method<static_cast<Numbers9>(&DebugDraw::ray)>(
            "ray", {"x", "y", "z", "dx", "dy", "dz", "r", "g", "b"});
        builder.Method<static_cast<Numbers9>(&DebugDraw::arrow)>(
            "arrow", {"x0", "y0", "z0", "x1", "y1", "z1", "r", "g", "b"});
        builder.Method<&DebugDraw::sphere>("sphere", {"x", "y", "z", "radius", "r", "g", "b"});
        builder.Method<static_cast<Numbers7>(&DebugDraw::cross)>(
            "cross", {"x", "y", "z", "size", "r", "g", "b"});
        builder.Method<static_cast<void (DebugDraw::*)(f32, f32, f32, String, f32, f32, f32) const>(
            &DebugDraw::text)>("text", {"x", "y", "z", "label", "r", "g", "b"});
        // Sedulous's by Float3 and Color, with overlay; each an arity family with ours.
        builder.Method<static_cast<Segment>(&DebugDraw::line)>("line", {"from", "to", "color"});
        builder.Method<static_cast<SegmentOverlay>(&DebugDraw::line)>("line",
                                                                      {"from", "to", "color", "overlay"});
        builder.Method<static_cast<Segment>(&DebugDraw::ray)>("ray", {"origin", "direction", "color"});
        builder.Method<static_cast<SegmentOverlay>(&DebugDraw::ray)>(
            "ray", {"origin", "direction", "color", "overlay"});
        builder.Method<static_cast<Segment>(&DebugDraw::wireBox)>("wireBox", {"min", "max", "color"});
        builder.Method<static_cast<SegmentOverlay>(&DebugDraw::wireBox)>(
            "wireBox", {"min", "max", "color", "overlay"});
        builder.Method<&DebugDraw::wireSphere>("wireSphere", {"center", "radius", "color"});
        builder.Method<static_cast<Marker>(&DebugDraw::cross)>("cross", {"center", "size", "color"});
        builder.Method<static_cast<MarkerOverlay>(&DebugDraw::cross)>(
            "cross", {"center", "size", "color", "overlay"});
        builder.Method<static_cast<Segment>(&DebugDraw::arrow)>("arrow", {"start", "end", "color"});
        builder.Method<static_cast<void (DebugDraw::*)(Float3, Float3, Color, f32) const>(
            &DebugDraw::arrow)>("arrow", {"start", "end", "color", "headSize"});
        builder.Method<static_cast<void (DebugDraw::*)(Float3, String, Color) const>(&DebugDraw::text)>(
            "text", {"worldPosition", "label", "color"});
        builder.Method<static_cast<void (DebugDraw::*)(f32, f32, String, Color) const>(
            &DebugDraw::screenText)>("screenText", {"x", "y", "label", "color"});
        builder.Method<static_cast<void (DebugDraw::*)(f32, f32, String, Color, f32) const>(
            &DebugDraw::screenText)>("screenText", {"x", "y", "label", "color", "scale"});
        builder.Method<&DebugDraw::of>("of", {"scene"});
        builder.Constructor();
    }

    REFLECT_ENUM(SpriteOrientation, "rtti::engine::render")
    {
        builder.Value("CameraFacing", SpriteOrientation::CameraFacing);
        builder.Value("CameraFacingY", SpriteOrientation::CameraFacingY);
        builder.Value("WorldAligned", SpriteOrientation::WorldAligned);
    }

    REFLECT_VALUE(SpriteComponent, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Sprite"))
            .Attribute("category", String(u8"Rendering"))
            .Method<&foundation::script::ComponentOf<SpriteComponent>, SpriteComponent>("of")
            .Property<&SpriteComponent::textureAsset>("texture")
            .Property<&SpriteComponent::size>("size")
            .Property<&SpriteComponent::uvRect>("uvRect")
            .Property<&SpriteComponent::tint>("tint")
            .Property<&SpriteComponent::orientation>("orientation")
            .Property<&SpriteComponent::additive>("additive")
            .Property<&SpriteComponent::visible>("visible");
    }

    REFLECT_VALUE(DecalComponent, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Decal"))
            .Attribute("category", String(u8"Rendering"))
            .Method<&foundation::script::ComponentOf<DecalComponent>, DecalComponent>("of")
            .Property<&DecalComponent::textureAsset>("texture")
            .Property<&DecalComponent::size>("size")
            .Property<&DecalComponent::color>("color")
            .Property<&DecalComponent::fadeStart>("fadeStart")
            .Property<&DecalComponent::fadeEnd>("fadeEnd")
            .Property<&DecalComponent::visible>("visible");
    }

    REFLECT_ENUM(SkyMode, "rtti::engine::render")
    {
        builder.Value("Procedural", SkyMode::Procedural);
        builder.Value("Analytic", SkyMode::Analytic);
        builder.Value("Color", SkyMode::Color);
        builder.Value("HDREquirect", SkyMode::HDREquirect);
        builder.Value("Cubemap", SkyMode::Cubemap);
    }

    REFLECT_ENUM(SettingsSource, "rtti::engine::render")
    {
        builder.Value("Scene", SettingsSource::Scene);
        builder.Value("Profile", SettingsSource::Profile);
    }

    REFLECT_VALUE(EnvironmentSettings, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Environment"))
            .Attribute("category", String(u8"Rendering"))
            // Script (Track A): EnvironmentSettings.of(scene) -> the scene's LIVE environment (edit
            // ambient/sky/fog fields). A scene-scoped re-resolving handle (the settings are one-per-scene).
            .Method<&EnvironmentSettingsOf, EnvironmentSettings>("of")
            .DataVersion(6) // v6: source + profile (v5: shadow reach; v4: IBL lighting dimmers)
            .ReadsDataVersionsFrom(4) // v4/v5 scenes read the defaults for what they lack
            .Property<&EnvironmentSettings::source>("source")
            .PropAttribute("sceneOnly", true)
            .PropAttribute("description",
                           String(u8"Where the values come from: this scene's own, or a shared "
                                  u8"Environment Profile asset"))
            .Property<&EnvironmentSettings::profile>("profile")
            .PropAttribute("sceneOnly", true)
            .PropAttribute("visibleWhen", String(u8"source=1"))
            .PropAttribute("description", String(u8"The shared Environment Profile whose values this scene uses"))
            .Property<&EnvironmentSettings::ambientColor>("ambientColor")
            .Property<&EnvironmentSettings::ambientIntensity>("ambientIntensity")
            .PropAttribute("range", Float4{0.0f, 2.0f, 0.01f, 0.0f})
            .PropAttribute(
                "description",
                String(
                    u8"Flat ambient fill added on top of the image-based ambient (0 = pure IBL)"))
            .Property<&EnvironmentSettings::skyMode>("skyMode")
            .Property<&EnvironmentSettings::skyTexture>("skyTexture")
            .PropAttribute("visibleWhen", String(u8"skyMode=3,4"))
            .PropAttribute("description",
                           String(u8"HDR (equirect) or cube texture for the textured sky modes"))
            .Property<&EnvironmentSettings::skyIntensity>("skyIntensity")
            .PropAttribute("range", Float4{0.0f, 10.0f, 0.05f, 0.0f})
            .PropAttribute(
                "description",
                String(u8"Environment radiance master: scales the sky AND the IBL lighting"))
            .Property<&EnvironmentSettings::skyBackgroundIntensity>("skyBackgroundIntensity")
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f})
            .PropAttribute("displayName", String(u8"Sky Background Intensity"))
            .PropAttribute("description",
                           String(u8"Dims only the VISIBLE sky backdrop; leaves the IBL lighting"))
            .Property<&EnvironmentSettings::skyRotation>("skyRotation")
            .PropAttribute("range", Float4{0.0f, 6.2832f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"skyMode=3,4"))
            .PropAttribute("description", String(u8"Sky yaw (radians)"))
            .Property<&EnvironmentSettings::skyHorizon>("skyHorizon")
            .PropAttribute("visibleWhen", String(u8"skyMode=0"))
            .Property<&EnvironmentSettings::skyZenith>("skyZenith")
            .PropAttribute("visibleWhen", String(u8"skyMode=0,2"))
            .PropAttribute("displayName", String(u8"Sky Zenith / Color"))
            .PropAttribute("description",
                           String(u8"Zenith color (procedural sky); the flat color in Color mode"))
            .Property<&EnvironmentSettings::skyGround>("skyGround")
            .PropAttribute("visibleWhen", String(u8"skyMode=0"))
            .Property<&EnvironmentSettings::sunIntensity>("sunIntensity")
            .PropAttribute("range", Float4{0.0f, 10.0f, 0.05f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"skyMode=0,1,2"))
            .Property<&EnvironmentSettings::sunAngularSize>("sunAngularSize")
            .PropAttribute("range", Float4{0.05f, 10.0f, 0.05f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"skyMode=0,1,2"))
            .PropAttribute("description", String(u8"Sun disc size (degrees)"))
            .Property<&EnvironmentSettings::turbidity>("turbidity")
            .PropAttribute("range", Float4{2.0f, 10.0f, 0.1f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"skyMode=1"))
            .PropAttribute("description", String(u8"Preetham haze (2 = clear, 10 = hazy)"))
            .Property<&EnvironmentSettings::iblDiffuseIntensity>("iblDiffuseIntensity")
            .PropAttribute("displayName", String(u8"IBL Diffuse Intensity"))
            .PropAttribute("range", Float4{0.0f, 2.0f, 0.01f, 0.0f})
            .PropAttribute("description",
                           String(u8"Sky lighting (diffuse) strength - dims the sky's color cast "
                                  u8"on surfaces without changing the visible sky"))
            .Property<&EnvironmentSettings::iblSpecularIntensity>("iblSpecularIntensity")
            .PropAttribute("displayName", String(u8"IBL Specular Intensity"))
            .PropAttribute("range", Float4{0.0f, 2.0f, 0.01f, 0.0f})
            .PropAttribute("description",
                           String(u8"Sky reflection strength on surfaces (probes keep their own "
                                  u8"intensity)"))
            .Property<&EnvironmentSettings::shadowDistance>("shadowDistance")
            .PropAttribute("range", Float4{5.0f, 1000.0f, 1.0f, 0.0f})
            .PropAttribute("description",
                           String(u8"How far from the camera the sun's shadows reach. Shorter keeps "
                                  u8"near shadows sharp (a roof's shadow on a wall); longer covers "
                                  u8"more ground"))
            .Property<&EnvironmentSettings::shadowCascadeSplit>("shadowCascadeSplit")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("description",
                           String(u8"How the shadow map is shared out over the reach: 0 = evenly, "
                                  u8"1 = most of it near the camera"))
            .Property<&EnvironmentSettings::shadowFadeDistance>("shadowFadeDistance")
            .PropAttribute("range", Float4{0.0f, 200.0f, 1.0f, 0.0f})
            .PropAttribute("description",
                           String(u8"The width shadows fade out over at the reach"));
    }

    REFLECT_VALUE(ReflectionProbeComponent, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Reflection Probe"))
            .Attribute("category", String(u8"Rendering"))
            .Method<&foundation::script::ComponentOf<ReflectionProbeComponent>, ReflectionProbeComponent>(
                "of")
            .Property<&ReflectionProbeComponent::halfExtents>("halfExtents")
            .Property<&ReflectionProbeComponent::blendDistance>("blendDistance")
            .PropAttribute("range", Float4{0.0f, 10.0f, 0.1f, 0.0f})
            .PropAttribute("description", String(u8"Fade width at the probe volume's edge"))
            .Property<&ReflectionProbeComponent::intensity>("intensity")
            .PropAttribute("range", Float4{0.0f, 5.0f, 0.05f, 0.0f})
            .Property<&ReflectionProbeComponent::resolution>("resolution")
            .Property<&ReflectionProbeComponent::priority>("priority")
            .Property<&ReflectionProbeComponent::update>("update")
            .Property<&ReflectionProbeComponent::parallax>("parallax")
            .Property<&ReflectionProbeComponent::enabled>("enabled");
    }

    REFLECT_ENUM(TonemapOperator, "rtti::engine::render")
    {
        builder.Value("Clamp", TonemapOperator::Clamp);
        builder.Value("AgX", TonemapOperator::AgX);
    }

    REFLECT_ENUM(AaMode, "rtti::engine::render")
    {
        builder.Value("Off", AaMode::Off);
        builder.Value("FXAA", AaMode::FXAA);
        builder.Value("TAA", AaMode::TAA);
    }

    REFLECT_ENUM(AoMode, "rtti::engine::render")
    {
        builder.Value("Off", AoMode::Off);
        builder.Value("GTAO", AoMode::GTAO);
        builder.Value("SSAO", AoMode::SSAO);
    }

    REFLECT_VALUE(PostProcessSettings, "rtti::engine::render")
    {
        builder.Attribute("displayName", String(u8"Post Processing"))
            .Attribute("category", String(u8"Rendering"))
            .DataVersion(4) // v4: source + profile (v3: SSGI; v2: auto-exposure + grading)
            .ReadsDataVersionsFrom(3) // a v3 scene reads source Scene
            // Script (Track A): PostProcessSettings.of(scene) -> the scene's LIVE post settings (edit
            // exposure/tonemap/bloom/AA). A scene-scoped re-resolving handle.
            .Method<&PostProcessSettingsOf, PostProcessSettings>("of")
            .Property<&PostProcessSettings::source>("source")
            .PropAttribute("sceneOnly", true)
            .PropAttribute("description",
                           String(u8"Where the values come from: this scene's own, or a shared "
                                  u8"Post Process Profile asset"))
            .Property<&PostProcessSettings::profile>("profile")
            .PropAttribute("sceneOnly", true)
            .PropAttribute("visibleWhen", String(u8"source=1"))
            .PropAttribute("description", String(u8"The shared Post Process Profile whose values this scene uses"))
            .Property<&PostProcessSettings::exposureEV>("exposureEV")
            .PropAttribute("range", Float4{-8.0f, 8.0f, 0.05f, 0.0f})
            .PropAttribute("displayName", String(u8"Exposure (EV)"))
            .PropAttribute("description",
                           String(u8"Exposure in stops; the tonemap applies 2^EV (0 = neutral)"))
            .Property<&PostProcessSettings::tonemapOperator>("tonemapOperator")
            .PropAttribute("displayName", String(u8"Tonemap"))
            .Property<&PostProcessSettings::bloomEnabled>("bloomEnabled")
            .PropAttribute("displayName", String(u8"Bloom"))
            .Property<&PostProcessSettings::bloomThreshold>("bloomThreshold")
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.01f, 0.0f})
            .Property<&PostProcessSettings::bloomKnee>("bloomKnee")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Property<&PostProcessSettings::bloomIntensity>("bloomIntensity")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.005f, 0.0f})
            .Property<&PostProcessSettings::autoExposure>("autoExposure")
            .PropAttribute("displayName", String(u8"Auto Exposure"))
            .PropAttribute("description",
                           String(u8"Exposure follows the scene's average luminance"))
            .Property<&PostProcessSettings::autoExposureKey>("autoExposureKey")
            .PropAttribute("range", Float4{0.02f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("displayName", String(u8"Auto Exposure Key"))
            .Property<&PostProcessSettings::autoExposureSpeed>("autoExposureSpeed")
            .PropAttribute("range", Float4{0.1f, 10.0f, 0.1f, 0.0f})
            .PropAttribute("displayName", String(u8"Adaptation Speed"))
            .Property<&PostProcessSettings::autoExposureMinEV>("autoExposureMinEV")
            .PropAttribute("range", Float4{-8.0f, 0.0f, 0.25f, 0.0f})
            .PropAttribute("displayName", String(u8"Auto Exposure Min (EV)"))
            .Property<&PostProcessSettings::autoExposureMaxEV>("autoExposureMaxEV")
            .PropAttribute("range", Float4{0.0f, 8.0f, 0.25f, 0.0f})
            .PropAttribute("displayName", String(u8"Auto Exposure Max (EV)"))
            .Property<&PostProcessSettings::gradingLut>("gradingLut")
            .PropAttribute("displayName", String(u8"Grading LUT"))
            .PropAttribute("description",
                           String(u8"Neutral strip LUT (256x16, Color Space = Linear), graded "
                                  u8"in an image editor"))
            .Property<&PostProcessSettings::gradingIntensity>("gradingIntensity")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("displayName", String(u8"Grading Intensity"))
            .Property<&PostProcessSettings::aoMode>("aoMode")
            .PropAttribute("displayName", String(u8"Ambient Occlusion"))
            .Property<&PostProcessSettings::aoStrength>("aoStrength")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"aoMode=1,2"))
            .PropAttribute("description", String(u8"Master AO mix (0 = none, 1 = full)"))
            .Property<&PostProcessSettings::aoRadius>("aoRadius")
            .PropAttribute("range", Float4{0.05f, 4.0f, 0.05f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"aoMode=1,2"))
            .Property<&PostProcessSettings::aoIntensity>("aoIntensity")
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"aoMode=1,2"))
            .Property<&PostProcessSettings::ssrEnabled>("ssrEnabled")
            .PropAttribute("displayName", String(u8"Screen-Space Reflections"))
            .Property<&PostProcessSettings::ssrIntensity>("ssrIntensity")
            .PropAttribute("range", Float4{0.0f, 2.0f, 0.02f, 0.0f})
            .Property<&PostProcessSettings::ssgiEnabled>("ssgiEnabled")
            .PropAttribute("displayName", String(u8"Screen-Space GI"))
            .PropAttribute("description",
                           String(u8"One temporal diffuse bounce gathered from the visible scene"))
            .Property<&PostProcessSettings::ssgiIntensity>("ssgiIntensity")
            .PropAttribute("range", Float4{0.0f, 3.0f, 0.02f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"ssgiEnabled"))
            .Property<&PostProcessSettings::aaMode>("aaMode")
            .PropAttribute("displayName", String(u8"Anti-Aliasing"))
            .Property<&PostProcessSettings::taaBlendFactor>("taaBlendFactor")
            .PropAttribute("range", Float4{0.5f, 0.99f, 0.005f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"aaMode=2"))
            .PropAttribute("description",
                           String(u8"TAA history weight (higher = steadier, more ghosting)"))
            .Property<&PostProcessSettings::taaVarianceGamma>("taaVarianceGamma")
            .PropAttribute("range", Float4{0.5f, 3.0f, 0.05f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"aaMode=2"))
            .Property<&PostProcessSettings::fxaaSubpixel>("fxaaSubpixel")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.05f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"aaMode=1"));
    }

} // namespace engine::render (reflection bodies)

namespace engine::render
{
    void RegisterRenderComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_LightType();
            RttiRegisterEnum_CameraProjection();
            RttiRegisterEnum_SpriteOrientation();
            RttiRegisterEnum_ShadowUpdateMode();
            RttiRegisterEnum_ProbeUpdateMode();
            RttiRegisterEnum_SkyMode();
            RttiRegisterEnum_SettingsSource();
            RttiRegisterValue_EnvironmentSettings();
            RttiRegisterEnum_TonemapOperator();
            RttiRegisterEnum_AaMode();
            RttiRegisterEnum_AoMode();
            RttiRegisterValue_PostProcessSettings();
            RegisterArrayType<foundation::resource::Ref<foundation::materials::Material>>();
            RttiRegisterValue_MeshComponent();
            RttiRegisterValue_InstancedMeshComponent();
            RttiRegisterValue_CameraComponent();
            RttiRegisterValue_LightComponent();
            RttiRegisterValue_SpriteComponent();
            RttiRegisterValue_DecalComponent();
            RttiRegisterValue_ReflectionProbeComponent();
            return true;
        }();
        (void)once;
    }

    void RegisterRenderScriptFacade()
    {
        RegisterRenderComponentReflection(); // ensure component TypeData (incl `of`) is built first
        // Surface the render COMPONENTS to script (Track A: MeshComponent.of(entity), ...): register
        // them (both backends emit registry types), seed the emission roots (nothing else
        // reaches an of()-only type), and make their class names import-visible in behavior preludes.
        // The WHOLE render component set - each `.of(entity)` exposes its editor-reflected properties.
        struct RenderComponentEntry
        {
            const core::TypeInfo* type;
            core::StringView name;
        };
        const RenderComponentEntry components[] = {
            {&core::TypeOf<MeshComponent>(), u8"MeshComponent"},
            {&core::TypeOf<InstancedMeshComponent>(), u8"InstancedMeshComponent"},
            {&core::TypeOf<CameraComponent>(), u8"CameraComponent"},
            {&core::TypeOf<LightComponent>(), u8"LightComponent"},
            {&core::TypeOf<SpriteComponent>(), u8"SpriteComponent"},
            {&core::TypeOf<DecalComponent>(), u8"DecalComponent"},
            {&core::TypeOf<ReflectionProbeComponent>(), u8"ReflectionProbeComponent"}};
        for (const RenderComponentEntry& component : components)
        {
            GlobalTypeRegistry().Register(*component.type);
            foundation::script::RegisterExtraScriptRootType(component.type);
            foundation::script::RegisterExtraFacadeName(component.name);
        }

        // The scene-bound render handle (SceneRender.of(scene)): reflect it, register it, seed the
        // emission root (nothing else reaches it), and make the class name prelude-visible.
        RttiRegisterValue_SceneRender();
        GlobalTypeRegistry().Register(core::TypeOf<SceneRender>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<SceneRender>());
        foundation::script::RegisterExtraFacadeName(u8"SceneRender");

        // DebugDraw.of(scene): immediate-mode debug draw into the scene's per-scene gizmo list
        // (reaches RenderSubsystem::DebugScene via the per-context render service the app installs).
        RttiRegisterValue_DebugDraw();
        GlobalTypeRegistry().Register(core::TypeOf<DebugDraw>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<DebugDraw>());
        foundation::script::RegisterExtraFacadeName(u8"DebugDraw");

        // The render scene-SYSTEM settings handles (EnvironmentSettings.of(scene) / PostProcess-
        // Settings.of(scene)): register + seed the emission root + name for the prelude. Their
        // TypeData (incl `of`) was built by RegisterRenderComponentReflection above.
        const core::TypeInfo* settings[] = {&core::TypeOf<EnvironmentSettings>(),
                                            &core::TypeOf<PostProcessSettings>()};
        const core::StringView settingsNames[] = {u8"EnvironmentSettings", u8"PostProcessSettings"};
        for (core::usize i = 0; i < 2; ++i)
        {
            GlobalTypeRegistry().Register(*settings[i]);
            foundation::script::RegisterExtraScriptRootType(settings[i]);
            foundation::script::RegisterExtraFacadeName(settingsNames[i]);
        }
    }
} // namespace engine::render
