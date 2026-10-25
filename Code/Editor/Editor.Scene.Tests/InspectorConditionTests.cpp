// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The inspector's property-attribute conventions (`visibleWhen`'s grammar, and the render
// components carrying it), moved out of EditContextTests as they were: they test the inspector's
// partition, still on foundation.ui, and come back with the inspector's layer
// (Documentation/Specs/editor-gui.md, 5.3), against the condition the GUI's grid reads.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <initializer_list>
#include "Core/Reflection/Reflect.h"

import foundation.core;
import engine.render;
import editor.scene;

using namespace foundation::core;
using namespace editor;

// --- Inspector property-attribute conventions ---

TEST_CASE("scene-edit: ParsePropertyCondition grammar")
{
    PropertyCondition c;

    // Truthy form.
    CHECK(ParsePropertyCondition(u8"castsShadows", c));
    CHECK(c.prop.AsView() == StringView(u8"castsShadows"));
    CHECK(c.values.IsEmpty());
    CHECK(MatchesPropertyCondition(c, 1));
    CHECK(!MatchesPropertyCondition(c, 0));

    // Value-list form.
    CHECK(ParsePropertyCondition(u8"type=1,2", c));
    CHECK(c.prop.AsView() == StringView(u8"type"));
    REQUIRE(c.values.Size() == 2u);
    CHECK(MatchesPropertyCondition(c, 1));
    CHECK(MatchesPropertyCondition(c, 2));
    CHECK(!MatchesPropertyCondition(c, 0));
    CHECK(!MatchesPropertyCondition(c, 3));

    CHECK(ParsePropertyCondition(u8"mode=-1", c));
    REQUIRE(c.values.Size() == 1u);
    CHECK(c.values[0] == -1);
    CHECK(MatchesPropertyCondition(c, -1));

    // Malformed specs refuse cleanly.
    CHECK(!ParsePropertyCondition(u8"", c));
    CHECK(!ParsePropertyCondition(u8"=1", c));
    CHECK(!ParsePropertyCondition(u8"type=", c));
    CHECK(!ParsePropertyCondition(u8"type=1,,2", c));
    CHECK(!ParsePropertyCondition(u8"type=x", c));
}

TEST_CASE("scene-edit: render components carry the inspector attribute annotations")
{
    engine::render::RegisterRenderComponentReflection();

    // Spot-only light angles hide behind the type condition; intensity gets a slider.
    const TypeInfo& light = TypeOf<engine::render::LightComponent>();
    const PropertyInfo* inner = FindProperty(light, "innerAngle");
    REQUIRE(inner != nullptr);
    const foundation::core::Attribute* vis = FindAttribute(*inner, u8"visibleWhen");
    REQUIRE(vis != nullptr);
    PropertyCondition c;
    REQUIRE(ParsePropertyCondition(vis->value.TryGet<String>()->AsView(), c));
    CHECK(c.prop.AsView() == StringView(u8"type"));
    CHECK(MatchesPropertyCondition(c, 2));  // Spot
    CHECK(!MatchesPropertyCondition(c, 0)); // Directional

    const PropertyInfo* intensity = FindProperty(light, "intensity");
    REQUIRE(intensity != nullptr);
    const foundation::core::Attribute* range = FindAttribute(*intensity, u8"range");
    REQUIRE(range != nullptr);
    CHECK(range->value.TryGet<Float4>() != nullptr);

    // The shadow controls show only while the light casts shadows.
    for (const char* name : {"shadowStrength", "shadowNormalBias", "shadowDepthBiasScale"})
    {
        const PropertyInfo* control = FindProperty(light, name);
        REQUIRE(control != nullptr);
        CHECK(FindAttribute(*control, u8"range") != nullptr);
        const foundation::core::Attribute* shadowVis = FindAttribute(*control, u8"visibleWhen");
        REQUIRE(shadowVis != nullptr);
        REQUIRE(ParsePropertyCondition(shadowVis->value.TryGet<String>()->AsView(), c));
        CHECK(c.prop.AsView() == StringView(u8"castsShadows"));
    }

    // Camera: the field of view shows for a perspective camera, the height for an orthographic.
    const TypeInfo& camera = TypeOf<engine::render::CameraComponent>();
    const PropertyInfo* fov = FindProperty(camera, "fovYRadians");
    REQUIRE(fov != nullptr);
    const foundation::core::Attribute* fovVis = FindAttribute(*fov, u8"visibleWhen");
    REQUIRE(fovVis != nullptr);
    REQUIRE(ParsePropertyCondition(fovVis->value.TryGet<String>()->AsView(), c));
    CHECK(c.prop.AsView() == StringView(u8"projection"));
    // ...and the mode itself is an enum by name (the inspector's dropdown, the schema's list).
    CHECK(TypeOf<engine::render::CameraProjection>().enumeratorCount == 2u);
    CHECK(MatchesPropertyCondition(c, 0));  // Perspective
    CHECK(!MatchesPropertyCondition(c, 1)); // Orthographic
    const PropertyInfo* orthoHeight = FindProperty(camera, "orthoHeight");
    REQUIRE(orthoHeight != nullptr);
    const foundation::core::Attribute* orthoVis = FindAttribute(*orthoHeight, u8"visibleWhen");
    REQUIRE(orthoVis != nullptr);
    REQUIRE(ParsePropertyCondition(orthoVis->value.TryGet<String>()->AsView(), c));
    CHECK(MatchesPropertyCondition(c, 1));
    CHECK(!MatchesPropertyCondition(c, 0));

    // Environment: turbidity is Analytic-only with a bounded slider.
    const TypeInfo& env = TypeOf<engine::render::EnvironmentSettings>();
    const PropertyInfo* turbidity = FindProperty(env, "turbidity");
    REQUIRE(turbidity != nullptr);
    CHECK(FindAttribute(*turbidity, u8"range") != nullptr);
    const foundation::core::Attribute* tvis = FindAttribute(*turbidity, u8"visibleWhen");
    REQUIRE(tvis != nullptr);
    REQUIRE(ParsePropertyCondition(tvis->value.TryGet<String>()->AsView(), c));
    CHECK(MatchesPropertyCondition(c, 1));  // Analytic
    CHECK(!MatchesPropertyCondition(c, 3)); // HDREquirect

    // Display-name override on the shared zenith/color slot.
    const PropertyInfo* zenith = FindProperty(env, "skyZenith");
    REQUIRE(zenith != nullptr);
    const foundation::core::Attribute* label = FindAttribute(*zenith, u8"displayName");
    REQUIRE(label != nullptr);
    CHECK(label->value.TryGet<String>()->AsView() == StringView(u8"Sky Zenith / Color"));
}
