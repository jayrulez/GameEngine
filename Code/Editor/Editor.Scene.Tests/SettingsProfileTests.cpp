// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Scene settings from a profile: while a block's source is a profile, an edit of a value field
// lands in the profile (the block's own fields stay the scene's) and undoes; Copy Into Scene and a
// source switch are one undo step each; Make Profile writes a profile asset from the block's values
// and switches to it; a profile-mode edit is queued for the save flow and written to the asset;
// the shared settings rows read where their access points and write through it; a scene previewed
// with no saved camera is framed whole. (The profile page's own tests are Editor.Render's.)
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.render;
import engine.render;
import engine.physics;
import engine.navigation;
import engine.script;
import pipeline.core;
import render.pipeline;
import editor.core;
import editor.scene;
import foundation.ui.toolkit; // the rows' editors

using namespace foundation::core;
using namespace engine::render;
namespace scene = foundation::scene;
using editor::SceneEditContext;

namespace
{
    const TypeInfo* EnvType() { return &TypeOf<EnvironmentSettings>(); }

    // A scene whose environment uses `profile` (a loaded product under `id`).
    struct ProfiledScene
    {
        scene::Scene scene{DefaultAllocator(), u8"Level"};
        EnvironmentSystem* env = nullptr;
        RefPtr<EnvironmentProfile> profile = MakeRef<EnvironmentProfile>(DefaultAllocator());
        editor::EditorCommandStack commands;
        SceneEditContext edit{scene, commands};
        Guid id{0x51u, 0x7Eu};

        ProfiledScene()
        {
            RegisterRenderComponentReflection();
            env = scene.AddSystem<EnvironmentSystem>();
            env->Environment().ambientIntensity = 0.25f;
            profile->values.ambientIntensity = 0.9f;
            env->Environment().source = SettingsSource::Profile;
            env->Environment().profile = profile.Get();
            env->Environment().profile.SetId(id);
        }
    };
}

TEST_CASE("settings profiles: a value edit lands in the profile, the block's own fields in the scene")
{
    ProfiledScene s;
    Array<Guid> edited;
    s.edit.OnSettingsProfileEdited = [&edited](const TypeInfo*, const Guid& profile)
    { edited.PushBack(profile); };

    s.edit.SetSceneSettingProperty(EnvType(), "ambientIntensity", Variant::From<f32>(0.7f));
    CHECK(s.profile->values.ambientIntensity == doctest::Approx(0.7f));
    CHECK(s.env->Environment().ambientIntensity == doctest::Approx(0.25f));
    REQUIRE(edited.Size() == 1);
    CHECK(edited[0] == s.id);

    // An enum, written raw, goes the same way.
    s.edit.SetSceneSettingPropertyRaw(EnvType(), "skyMode",
                                      static_cast<i64>(foundation::render::SkyMode::Analytic));
    CHECK(s.profile->values.skyMode == foundation::render::SkyMode::Analytic);

    s.commands.Undo();
    s.commands.Undo();
    CHECK(s.profile->values.ambientIntensity == doctest::Approx(0.9f));
    CHECK(s.profile->values.skyMode != foundation::render::SkyMode::Analytic);
    CHECK(edited.Size() == 4); // every write to the profile is persisted, the undos too

    // The source is the scene's ("sceneOnly"): switching it edits the block, and after it an edit
    // lands in the scene's own values.
    s.edit.SetSceneSettingPropertyRaw(EnvType(), "source", static_cast<i64>(SettingsSource::Scene));
    CHECK(s.env->Environment().source == SettingsSource::Scene);
    s.edit.SetSceneSettingProperty(EnvType(), "ambientIntensity", Variant::From<f32>(0.4f));
    CHECK(s.env->Environment().ambientIntensity == doctest::Approx(0.4f));
    CHECK(s.profile->values.ambientIntensity == doctest::Approx(0.9f));
    CHECK(edited.Size() == 4);
    s.commands.Undo();
    s.commands.Undo();
    CHECK(s.env->Environment().source == SettingsSource::Profile);
    CHECK(s.env->Environment().ambientIntensity == doctest::Approx(0.25f));
}

TEST_CASE("settings profiles: Copy Into Scene and a source switch are one undo step each")
{
    ProfiledScene s;
    s.profile->values.turbidity = 6.0f;

    REQUIRE(s.edit.MutateSceneSettings(EnvType(), [](scene::SceneSystem& system)
                                       { system.CopySettingsProfileIntoScene(); }));
    CHECK(s.env->Environment().source == SettingsSource::Scene);
    CHECK(s.env->Environment().ambientIntensity == doctest::Approx(0.9f));
    CHECK(s.env->Environment().turbidity == doctest::Approx(6.0f));
    CHECK(s.env->Environment().profile.id == s.id); // kept: the profile is a pick away
    s.commands.Undo();
    CHECK(s.env->Environment().source == SettingsSource::Profile);
    CHECK(s.env->Environment().ambientIntensity == doctest::Approx(0.25f));

    const Guid other{0x99u, 0x1u};
    REQUIRE(s.edit.MutateSceneSettings(EnvType(), [other](scene::SceneSystem& system)
                                       { system.UseSettingsProfile(other); }));
    CHECK(s.env->Environment().profile.id == other);
    s.commands.Undo();
    CHECK(s.env->Environment().profile.id == s.id);
}

TEST_CASE("settings profiles: Make Profile writes the values and switches; an edit reaches the asset")
{
    pipeline::RegisterRenderProfileAssets();
    RegisterRenderComponentReflection();
    const StringView dir = u8"scratch_editor_settings_profiles";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    {
        editor::EditorContext context(DefaultAllocator());
        context.SetProject(project.Get());
        pipeline::RegisterRenderCreators(context.Creators());
        // The join the app wires from its builders: the environment profile's asset.
        context.SourceAssetTypesOf = [](const TypeInfo& product)
        {
            Array<const TypeInfo*> out;
            if (&product == &EnvironmentProfile::StaticType())
            {
                out.PushBack(&pipeline::EnvironmentProfileAsset::StaticType());
            }
            return out;
        };

        scene::Scene scene(DefaultAllocator(), u8"Level");
        auto* env = scene.AddSystem<EnvironmentSystem>();
        env->Environment().ambientIntensity = 0.3f;
        env->Environment().turbidity = 5.0f;
        editor::EditorCommandStack commands;
        SceneEditContext edit(scene, commands);

        foundation::content::Instance* made =
            editor::MakeSettingsProfile(context, edit, EnvType(), u8"Level Environment");
        REQUIRE(made != nullptr);
        CHECK(made->Path() == u8"Profiles/Level Environment");
        const Guid id = made->Id();
        {
            RefPtr<ISerializable> object = made->ReadObject();
            auto* asset = Cast<pipeline::EnvironmentProfileAsset>(object.Get());
            REQUIRE(asset != nullptr);
            CHECK(asset->values.ambientIntensity == doctest::Approx(0.3f));
            CHECK(asset->values.turbidity == doctest::Approx(5.0f));
            CHECK(asset->values.source == SettingsSource::Scene); // a profile has no source
        }
        CHECK(env->Environment().source == SettingsSource::Profile);
        CHECK(env->Environment().profile.id == id);
        commands.Undo();
        CHECK(env->Environment().source == SettingsSource::Scene);
        commands.Redo();

        // The profile loaded (here by hand; the editor binds the cooked product): an edit in
        // profile mode is queued and the save flow writes it to the asset.
        RefPtr<EnvironmentProfile> product = MakeRef<EnvironmentProfile>(DefaultAllocator());
        product->values = env->Environment();
        env->Environment().profile = product.Get();
        env->Environment().profile.SetId(id);
        edit.OnSettingsProfileEdited = [&](const TypeInfo* type, const Guid& profile)
        {
            if (scene::SceneSystem* system = edit.FindSystemBySettingsType(type))
            {
                editor::QueueSettingsProfileEdit(context, *system, profile);
            }
        };
        edit.SetSceneSettingProperty(EnvType(), "turbidity", Variant::From<f32>(8.0f));
        CHECK(product->values.turbidity == doctest::Approx(8.0f));
        REQUIRE(context.HasPendingAssetEdits());
        REQUIRE(context.DrainAssetEdits(project->SourceDb()).IsOk());
        {
            RefPtr<ISerializable> object = project->SourceDb().GetInstance(id)->ReadObject();
            auto* asset = Cast<pipeline::EnvironmentProfileAsset>(object.Get());
            REQUIRE(asset != nullptr);
            CHECK(asset->values.turbidity == doctest::Approx(8.0f));
            CHECK(asset->values.ambientIntensity == doctest::Approx(0.3f));
        }
        context.SetProject(nullptr);
    }
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

namespace
{
    // The editor of the row named `name` (the reflected field name), or null.
    template <typename E>
    E* RowNamed(foundation::ui::toolkit::PropertyGrid& grid, StringView name)
    {
        for (usize i = 0; i < grid.PropertyCount(); ++i)
        {
            if (grid.PropertyAt(i)->Name() == name)
            {
                return Cast<E>(grid.PropertyAt(i));
            }
        }
        return nullptr;
    }
}

TEST_CASE("settings profiles: the shared rows read where their access points and write through it")
{
    RegisterRenderComponentReflection();
    editor::EditorContext context(DefaultAllocator());
    RefPtr<foundation::ui::toolkit::PropertyGrid> grid =
        MakeRef<foundation::ui::toolkit::PropertyGrid>(DefaultAllocator());
    Array<Function<void()>> refreshers;

    EnvironmentSettings first;
    EnvironmentSettings second;
    first.turbidity = 3.0f;
    second.turbidity = 9.0f;
    EnvironmentSettings* shown = &first;
    Array<String> written;
    auto access = MakeRef<editor::SettingsAccess>(DefaultAllocator());
    access->type = EnvType();
    access->values = [&shown](const char*) -> void* { return shown; };
    access->set = [&written](const char* property, const Variant&)
    { written.PushBack(String(reinterpret_cast<const utf8char*>(property))); };

    editor::SettingsRows rows(context, *grid, refreshers);
    usize shownFields = 0;
    for (const PropertyInfo& prop : Properties(*EnvType()))
    {
        if (IsNested(prop) || FindAttribute(prop, u8"sceneOnly") != nullptr)
        {
            continue;
        }
        rows.Build(access, prop, u8"Environment");
        ++shownFields;
    }
    CHECK(grid->PropertyCount() == shownFields); // one row per value field

    auto* turbidity = RowNamed<foundation::ui::toolkit::RangeEditor>(*grid, u8"turbidity");
    REQUIRE(turbidity != nullptr);
    CHECK(turbidity->Value() == doctest::Approx(3.0f));
    shown = &second; // the values moved (a block's source switched): the next refresh follows
    for (const Function<void()>& refresh : refreshers)
    {
        refresh();
    }
    CHECK(turbidity->Value() == doctest::Approx(9.0f));
    REQUIRE(static_cast<bool>(turbidity->Setter));
    turbidity->Setter(4.0f);
    REQUIRE(written.Size() == 1);
    CHECK(written[0] == u8"turbidity");

    // A host's own row starts in its current state (no frame showing its built-in label).
    auto button = MakeRef<foundation::ui::toolkit::ButtonEditor>(DefaultAllocator(), StringView(u8"Open"),
                                                                 Function<void()>{}, StringView(u8"Environment"));
    foundation::ui::toolkit::ButtonEditor* raw = button.Get();
    rows.AddEditor(raw, [raw]() { raw->SetButtonEnabled(false); });
    CHECK_FALSE(raw->ButtonEnabled());
}

namespace
{
    // Measures two entities as fixed boxes, as a render or physics domain answers.
    class TwoBoxes final : public scene::SceneSystem, public scene::ISceneEntityBounds
    {
    public:
        scene::EntityHandle a = scene::EntityHandle::Invalid();
        scene::EntityHandle b = scene::EntityHandle::Invalid();
        [[nodiscard]] scene::ISceneEntityBounds* AsEntityBounds() noexcept override { return this; }
        [[nodiscard]] bool EntityBounds(scene::Scene&, scene::EntityHandle e, AABB& out) override
        {
            if (e == a)
            {
                out = AABB{Float3{0.0f, 0.0f, 0.0f}, Float3{1.0f, 1.0f, 1.0f}};
                return true;
            }
            if (e == b)
            {
                out = AABB{Float3{4.0f, -2.0f, 0.0f}, Float3{5.0f, 0.0f, 3.0f}};
                return true;
            }
            return false;
        }
    };
}

// A previewed scene with no saved camera is framed whole: the bounds of everything measured.
TEST_CASE("settings profiles: a scene's world bounds merge every measured entity")
{
    scene::Scene scene(DefaultAllocator(), u8"level");
    AABB bounds;
    CHECK_FALSE(editor::SceneWorldBounds(scene, bounds)); // nothing has a size
    auto* boxes = scene.AddSystem<TwoBoxes>();
    boxes->a = scene.CreateEntity(u8"A");
    boxes->b = scene.CreateEntity(u8"B");
    (void)scene.CreateEntity(u8"Marker"); // measured by nothing
    REQUIRE(editor::SceneWorldBounds(scene, bounds));
    CHECK(bounds.min.x == doctest::Approx(0.0f));
    CHECK(bounds.min.y == doctest::Approx(-2.0f));
    CHECK(bounds.max.x == doctest::Approx(5.0f));
    CHECK(bounds.max.z == doctest::Approx(3.0f));
}

// The Scene tab titles each settings section with the displayName its type declares, not its C++
// name ("PhysicsScene", "PostProcess" read as code to a user); a type without one keeps its name
// minus "Settings".
TEST_CASE("inspector: a scene settings section is titled with its type's display name")
{
    RegisterRenderComponentReflection();
    engine::physics::RegisterPhysicsComponentReflection();
    engine::navigation::RegisterNavigationComponentReflection();
    engine::script::RegisterScriptComponentReflection();

    CHECK(editor::SettingsCategoryName(TypeOf<EnvironmentSettings>()) == StringView(u8"Environment"));
    CHECK(editor::SettingsCategoryName(TypeOf<PostProcessSettings>()) ==
          StringView(u8"Post Processing"));
    CHECK(editor::SettingsCategoryName(TypeOf<engine::physics::PhysicsSceneSettings>()) ==
          StringView(u8"Physics"));
    CHECK(editor::SettingsCategoryName(TypeOf<engine::navigation::NavigationSceneSettings>()) ==
          StringView(u8"Navigation"));
    CHECK(editor::SettingsCategoryName(TypeOf<engine::script::SceneScriptSettings>()) ==
          StringView(u8"Scene Script"));
}
