// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The settings profile page: its preview takes the profile's values, on the built-in scene or on a
// project scene that takes a profile, and the scene chosen to preview on is kept per profile.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import engine.render;
import render.pipeline;
import editor.render;

using namespace foundation::core;
using namespace engine::render;
namespace scene = foundation::scene;

TEST_CASE("settings profiles: the profile page's preview takes the profile's values")
{
    RegisterRenderComponentReflection();
    pipeline::RegisterRenderProfileAssets();
    scene::Scene scene(DefaultAllocator(), u8"profile.preview");
    auto* env = scene.AddSystem<EnvironmentSystem>();
    auto* post = scene.AddSystem<PostProcessSystem>();
    env->Environment().profile.SetId(Guid{0x1u, 0x2u});

    RefPtr<pipeline::EnvironmentProfileAsset> environment =
        MakeRef<pipeline::EnvironmentProfileAsset>(DefaultAllocator());
    environment->values.turbidity = 7.0f;
    environment->values.shadowDistance = 45.0f;
    CHECK(editor::ApplySettingsProfileToScene(*environment, scene, nullptr));
    CHECK(env->Environment().turbidity == doctest::Approx(7.0f));
    CHECK(env->Environment().shadowDistance == doctest::Approx(45.0f));
    CHECK(env->Environment().source == SettingsSource::Scene); // the block's own fields kept
    CHECK(env->Environment().profile.id == Guid{0x1u, 0x2u});

    RefPtr<pipeline::PostProcessProfileAsset> look =
        MakeRef<pipeline::PostProcessProfileAsset>(DefaultAllocator());
    look->values.exposureEV = 1.25f;
    CHECK(editor::ApplySettingsProfileToScene(*look, scene, nullptr));
    CHECK(post->Post().exposureEV == doctest::Approx(1.25f));

    scene::Scene empty(DefaultAllocator(), u8"empty");
    CHECK_FALSE(editor::ApplySettingsProfileToScene(*look, empty, nullptr)); // no block to take them
}

// A profile previewed on one of the project's scenes (user 2026-10-09): a scene whose block takes a
// profile by reference renders that profile's cooked values, so the preview turns the block to its
// own values before giving it the ones being edited.
TEST_CASE("settings profiles: previewed on a scene that takes a profile, the edited values show")
{
    RegisterRenderComponentReflection();
    pipeline::RegisterRenderProfileAssets();
    scene::Scene scene(DefaultAllocator(), u8"level");
    auto* env = scene.AddSystem<EnvironmentSystem>();
    env->UseSettingsProfile(Guid{0x1u, 0x2u});
    REQUIRE(env->Environment().source == SettingsSource::Profile);

    RefPtr<pipeline::EnvironmentProfileAsset> environment =
        MakeRef<pipeline::EnvironmentProfileAsset>(DefaultAllocator());
    environment->values.turbidity = 6.5f;
    CHECK(editor::PreviewSettingsProfileInScene(*environment, scene, nullptr));
    CHECK(env->Environment().source == SettingsSource::Scene);
    CHECK(env->Effective().turbidity == doctest::Approx(6.5f)); // what the renderer reads
}

// The scene a profile is previewed on is remembered per profile, in the project's editor store.
TEST_CASE("settings profiles: the preview scene choice round-trips per profile")
{
    editor::ProfilePreviewSettings settings;
    settings.prefs.PushBack(editor::ProfilePreviewPref{Guid{0x1u, 0x1u}, Guid{0x2u, 0x2u}});
    settings.prefs.PushBack(editor::ProfilePreviewPref{Guid{0x3u, 0x3u}, Guid{}});
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        settings.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    editor::ProfilePreviewSettings read;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        read.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(read.prefs.Size() == 2u);
    CHECK(read.prefs[0].profile == Guid{0x1u, 0x1u});
    CHECK(read.prefs[0].scene == Guid{0x2u, 0x2u});
    CHECK(read.prefs[1].scene.IsNil()); // the built-in scene
}
