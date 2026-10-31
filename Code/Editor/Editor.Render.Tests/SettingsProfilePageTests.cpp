// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The settings profile page: its preview takes the profile's values, on the built-in scene or on a
// project scene that takes a profile, and the scene chosen to preview on is kept per profile. Then
// the page built from the preview pages' markup, over a profile in a project and without a host (so
// no preview): its rows are the values without the scene's own fields, an edit is one undo step,
// and Discard Changes takes every edit back.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.scene;
import foundation.gui;
import foundation.gui.toolkit;
import engine.render;
import render.pipeline;
import editor.core;
import editor.app;
import editor.render;

using namespace foundation::core;
using namespace engine::render;
namespace scene = foundation::scene;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    // A project with an environment profile ("Dusk") and its editor settings.
    struct Bed
    {
        String dir = String(u8"scratch_profile_page_project");
        UniquePtr<editor::EditorProject> project;
        foundation::settings::Settings store{DefaultAllocator()};
        editor::app::AssetPageServicesSource none; // no services for the registration; outlives the context below
        editor::EditorContext context{DefaultAllocator()};
        foundation::content::Instance* dusk = nullptr;

        Bed()
        {
            RegisterRenderComponentReflection();
            pipeline::RegisterRenderProfileAssets();
            editor::RegisterSettingsProfileEditor(context, none); // registers the prefs' section
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
            dusk = project->SourceDb().RootGroup()->CreateInstance(u8"Dusk", pipeline::EnvironmentProfileAsset::StaticType());
            REQUIRE(dusk != nullptr);
            pipeline::EnvironmentProfileAsset asset;
            asset.values.turbidity = 3.0f;
            REQUIRE(dusk->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<editor::SettingsProfilePage> Open()
        {
            editor::app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<editor::SettingsProfilePage>(DefaultAllocator(), context, Move(services), *dusk);
        }
    };

    [[nodiscard]] bool HasRow(gui::IPropertySource& rows, StringView name)
    {
        Array<gui::PropertyInfo> top;
        rows.GetProperties(StringView{}, top);
        for (const gui::PropertyInfo& info : top)
            if (info.Name == name)
                return true;
        return false;
    }
}

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

// A profile previewed on one of the project's scenes: a scene whose block takes a
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

TEST_CASE("settings profile page: its rows are the values', the scene's own fields left out")
{
    Bed bed;
    UniquePtr<editor::SettingsProfilePage> page = bed.Open();
    REQUIRE_FALSE(page->LoadFailed());
    gui::IPropertySource& rows = page->Rows();
    CHECK(rows.ObjectCount() == 1u);
    CHECK(HasRow(rows, u8"turbidity"));
    CHECK_FALSE(HasRow(rows, u8"source")); // belongs to a scene, not a profile
    CHECK_FALSE(HasRow(rows, u8"profile"));
    CHECK(rows.GetValue(u8"turbidity", 0).AsFloat() == doctest::Approx(3.0));
    CHECK(page->PreviewSceneId().IsNil()); // the built-in scene

    // The bar names the kind by the asset type's display name, each profile declaring its own.
    CHECK(TypeAttrString(pipeline::EnvironmentProfileAsset::StaticType(), "displayName", StringView{}) == StringView(u8"Environment Profile"));
    CHECK(TypeAttrString(pipeline::PostProcessProfileAsset::StaticType(), "displayName", StringView{}) == StringView(u8"Post Process Profile"));
}

TEST_CASE("settings profile page: an edit is one undo step; Discard Changes takes every edit back")
{
    Bed bed;
    UniquePtr<editor::SettingsProfilePage> page = bed.Open();
    gui::IPropertySource& rows = page->Rows();
    page->EditValue(u8"turbidity", gui::PropertyWrite{gui::Variant(5.0)});
    CHECK(page->IsDirty());
    CHECK(rows.GetValue(u8"turbidity", 0).AsFloat() == doctest::Approx(5.0));
    page->Commands().Undo();
    CHECK(rows.GetValue(u8"turbidity", 0).AsFloat() == doctest::Approx(3.0));

    page->EditValue(u8"turbidity", gui::PropertyWrite{gui::Variant(6.0)});
    page->EditValue(u8"turbidity", gui::PropertyWrite{gui::Variant(7.0)});
    page->DiscardChanges();
    CHECK_FALSE(page->IsDirty());
    CHECK(rows.GetValue(u8"turbidity", 0).AsFloat() == doctest::Approx(3.0));
}

TEST_CASE("settings profile page: a gesture's previews reach the preview as they go, and its cancel")
{
    Bed bed;
    UniquePtr<editor::SettingsProfilePage> page = bed.Open();
    gui::IPropertySource& rows = page->Rows();
    const usize before = page->PreviewsApplied();
    // Like a slider's drag: each value is written in place and given to the preview.
    rows.BeginEdit(u8"turbidity");
    rows.PreviewEdit(u8"turbidity", gui::PropertyWrite{gui::Variant(4.0)});
    CHECK(page->PreviewsApplied() == before + 1u);
    CHECK(rows.GetValue(u8"turbidity", 0).AsFloat() == doctest::Approx(4.0));
    rows.PreviewEdit(u8"turbidity", gui::PropertyWrite{gui::Variant(5.0)});
    CHECK(page->PreviewsApplied() == before + 2u);
    CHECK_FALSE(page->Commands().CanUndo()); // no step while it goes
    rows.CancelEdit(u8"turbidity");
    CHECK(page->PreviewsApplied() == before + 3u); // the value back, the preview with it
    CHECK(rows.GetValue(u8"turbidity", 0).AsFloat() == doctest::Approx(3.0));
}
