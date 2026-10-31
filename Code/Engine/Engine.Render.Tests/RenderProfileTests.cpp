// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Render profiles: a scene's environment and post settings take their values from the scene or
// from a shared profile, chosen by the block's source. The values in effect drive extraction,
// post resolution and the scene's script handle; a missing profile falls back to the scene's own;
// the blocks round-trip and read their earlier versions as source Scene; a cooked profile loads
// through its factory.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.render;
import engine.render;
import foundation.script.facades; // script::Scene (the settings handle's argument)

using namespace foundation::core;
using namespace engine::render;
namespace scene = foundation::scene;

namespace
{
    void RemoveTree(StringView root)
    {
        foundation::vfs::NativeFileSystem fs(root, DefaultAllocator());
        Array<foundation::vfs::DirEntry> entries;
        if (fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
        {
            for (const auto& e : entries)
            {
                if (!e.isDirectory)
                {
                    (void)fs.AsWritable()->Delete(e.name.AsView());
                }
            }
        }
        (void)RemoveDirectory(root);
    }

    // Writes a settings payload under an explicit version chain, as an older scene stored it.
    template <typename Body>
    MemoryStream WriteAtVersion(const TypeInfo& type, u32 version, Body&& body)
    {
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, version}};
        u32 n = 1;
        ar.Key("dataVersions");
        ar.BeginArray(n);
        SerializedDataVersion entry = chain[0];
        ar.Key("type");
        ar.Scalar(&entry.typeId, ScalarKind::UInt64);
        ar.Key("version");
        ar.Scalar(&entry.version, ScalarKind::UInt32);
        ar.EndArray();
        ar.PushVersionScope(chain, 1);
        body(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
        (void)stream.Seek(0, SeekOrigin::Begin);
        return stream;
    }
}

TEST_CASE("render profiles: the source chooses the values in effect, for extraction and post")
{
    RegisterRenderComponentReflection();
    scene::Scene scene(DefaultAllocator(), u8"profiles");
    auto* env = scene.AddSystem<EnvironmentSystem>();
    auto* post = scene.AddSystem<PostProcessSystem>();
    env->Environment().ambientIntensity = 0.25f;
    post->Post().exposureEV = 0.5f;

    RefPtr<EnvironmentProfile> envProfile = MakeRef<EnvironmentProfile>(DefaultAllocator());
    envProfile->values.ambientIntensity = 0.9f;
    envProfile->values.shadowDistance = 60.0f;
    RefPtr<PostProcessProfile> postProfile = MakeRef<PostProcessProfile>(DefaultAllocator());
    postProfile->values.exposureEV = 1.5f;
    env->Environment().profile = envProfile.Get(); // a direct override (the picker binds by guid)
    post->Post().profile = postProfile.Get();

    // Scene source: the profile is referenced but not used.
    CHECK(env->Effective().ambientIntensity == doctest::Approx(0.25f));
    CHECK(post->Effective().exposureEV == doctest::Approx(0.5f));

    // Profile source: the profile's values are in effect everywhere.
    env->Environment().source = SettingsSource::Profile;
    post->Post().source = SettingsSource::Profile;
    CHECK(env->Effective().ambientIntensity == doctest::Approx(0.9f));
    CHECK(post->Effective().exposureEV == doctest::Approx(1.5f));
    foundation::render::ExtractedScene out{DefaultAllocator()};
    ExtractEnvironmentInto(scene, out);
    CHECK(out.ShadowSettings().distance == doctest::Approx(60.0f));
    CHECK(ResolveScenePost(post->Effective()).exposure == doctest::Approx(Pow(2.0f, 1.5f)));

    // The scene's script handle edits what is in effect: the loaded profile.
    Variant handle = EnvironmentSettingsOf(foundation::script::Scene{&scene});
    auto* settings = static_cast<EnvironmentSettings*>(handle.Resolve());
    REQUIRE(settings != nullptr);
    CHECK(settings == &envProfile->values);

    // A profile that is not there: the scene's own values stand in.
    env->Environment().profile = foundation::resource::Ref<EnvironmentProfile>{};
    CHECK(env->Effective().ambientIntensity == doctest::Approx(0.25f));
}

TEST_CASE("render profiles: the blocks round-trip their source, and older blocks read source Scene")
{
    RegisterRenderComponentReflection();
    const TypeInfo& envType = TypeOf<EnvironmentSettings>();
    const TypeInfo& postType = TypeOf<PostProcessSettings>();
    CHECK(envType.dataVersion == 6u);
    CHECK(postType.dataVersion == 4u);

    // Current version: the source and the profile's id survive.
    EnvironmentSystem written;
    written.Environment().source = SettingsSource::Profile;
    written.Environment().profile.SetId(Guid{0x12u, 0x34u});
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, envType);
        written.SerializeSettings(ar);
        EndVersionedPayload(ar);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    EnvironmentSystem read;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, envType);
        read.SerializeSettings(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(read.Environment().source == SettingsSource::Profile);
    CHECK(read.Environment().profile.id == Guid{0x12u, 0x34u});

    // A version 5 environment (no source) and a version 3 post block read as their scene's own values.
    EnvironmentSettings older;
    older.ambientIntensity = 0.4f;
    MemoryStream v5 = WriteAtVersion(envType, 5, [&](ISerializer& ar)
                                     { SerializeEnvironmentValues(ar, older, true); });
    EnvironmentSystem fromV5;
    {
        BinarySerializer ar(v5, SerializeMode::Read);
        BeginVersionedPayload(ar, envType);
        fromV5.SerializeSettings(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(fromV5.Environment().source == SettingsSource::Scene);
    CHECK(fromV5.Environment().ambientIntensity == doctest::Approx(0.4f));

    PostProcessSettings olderPost;
    olderPost.exposureEV = -1.0f;
    MemoryStream v3 = WriteAtVersion(postType, 3, [&](ISerializer& ar)
                                     { SerializePostValues(ar, olderPost); });
    PostProcessSystem fromV3;
    {
        BinarySerializer ar(v3, SerializeMode::Read);
        BeginVersionedPayload(ar, postType);
        fromV3.SerializeSettings(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(fromV3.Post().source == SettingsSource::Scene);
    CHECK(fromV3.Post().exposureEV == doctest::Approx(-1.0f));
}

TEST_CASE("render profiles: a cooked profile loads through its factory")
{
    RegisterRenderProfileResources();
    const StringView dir = u8"scratch_render_profiles_db";
    RemoveTree(dir);
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                                u8".rasset");
        auto* envInstance =
            db.RootGroup()->CreateInstance(u8"dusk", EnvironmentProfileSource::StaticType());
        auto* postInstance =
            db.RootGroup()->CreateInstance(u8"punchy", PostProcessProfileSource::StaticType());
        EnvironmentProfileSource envSource;
        envSource.values.skyMode = foundation::render::SkyMode::Analytic;
        envSource.values.turbidity = 7.0f;
        envSource.values.shadowFadeDistance = 12.0f;
        REQUIRE(envInstance->WriteObject(envSource).IsOk());
        PostProcessProfileSource postSource;
        postSource.values.aaMode = AaMode::FXAA;
        postSource.values.bloomIntensity = 0.2f;
        REQUIRE(postInstance->WriteObject(postSource).IsOk());

        EnvironmentProfileFactory envFactory(DefaultAllocator());
        PostProcessProfileFactory postFactory(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&envFactory);
        manager.AddFactory(&postFactory);
        foundation::resource::Proxy<EnvironmentProfile> envProfile =
            manager.Bind<EnvironmentProfile>(envInstance->Id());
        foundation::resource::Proxy<PostProcessProfile> postProfile =
            manager.Bind<PostProcessProfile>(postInstance->Id());
        REQUIRE(envProfile);
        REQUIRE(postProfile);
        CHECK(envProfile->values.skyMode == foundation::render::SkyMode::Analytic);
        CHECK(envProfile->values.turbidity == doctest::Approx(7.0f));
        CHECK(envProfile->values.shadowFadeDistance == doctest::Approx(12.0f));
        CHECK(postProfile->values.aaMode == AaMode::FXAA);
        CHECK(postProfile->values.bloomIntensity == doctest::Approx(0.2f));
    }
    RemoveTree(dir);
}
