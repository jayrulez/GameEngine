// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The scene editor's checks from before the GUI rewrite, on the new code: the camera's wheel dolly
// keeps its pivot; the Scene creator names its scenes uniquely and seeds each with a Sun; a copy the
// user asks for says what it copied; the hierarchy's double-click frames, its arrow keys move the
// selection, its Copy ID puts the guid on the clipboard; a v3 view pref reads through the legacy
// gate; the frame-rate readout; scene settings from a profile (an edit lands in the profile, Copy
// Into Scene and a source switch one undo step each, Make Profile writes the asset and switches);
// the settings source reads where the block's values are; a scene's world bounds; the settings
// sections titled by display name; the render components' inspector attributes as the reflected
// source evaluates them (the collision groups' cap is in ScenePageTests, beside the groups).

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.content;
import foundation.resource;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import foundation.settings;
import foundation.scene;
import foundation.scene.resource;
import foundation.render; // SkyMode
import foundation.physics; // kCollisionGroupCount
import engine.scene;
import engine.render;
import engine.composition; // FullSceneComposition
import engine.physics;
import engine.navigation;
import engine.script;
import foundation.gui;
import foundation.gui.toolkit;
import pipeline.core;   // AssetCreatorRegistry
import scene.pipeline;  // the Scene creator
import render.pipeline; // the profile assets and their creators
import editor.core;
import editor.app;
import editor.camera;
import editor.scene;

using namespace foundation::core;
using namespace editor;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace gui = foundation::gui;
namespace shell = foundation::shell;

namespace
{
    // A host whose context has a scene subsystem and nothing else: no renderer, no window.
    class SceneHost final : public runtime::IApplicationHost
    {
    public:
        SceneHost()
        {
            engine::RegisterAllSceneComponentReflection();
            m_context.AddSubsystem<engine::scene::SceneSubsystem>()->SetComposition(engine::FullSceneComposition());
            m_context.Startup();
        }
        ~SceneHost() override { m_context.Shutdown(); }
        runtime::Context& Ctx() noexcept override { return m_context; }
        shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const shell::WindowSettings&, const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::Context m_context{DefaultAllocator()};
    };

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

    // A project with a scene ("Level", a Crate in it), and the page over it.
    struct Bed
    {
        String dir = String(u8"scratch_scene_parity_project");
        UniquePtr<EditorProject> project;
        foundation::settings::Settings store{DefaultAllocator()};
        EditorContext context{DefaultAllocator()};
        SceneHost host;
        foundation::content::Instance* level = nullptr;
        Guid crate;

        Bed()
        {
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            RegisterSceneViewSettingsType();
            context.SetProjectEditorSettings(&store);
            RegisterSceneEditorActions(context);
            app::FileActions file;
            file.save = [](EditorPage& page) { (void)page.Save(); };
            app::RegisterFileActions(context.Actions(), Move(file));
            app::RegisterEditActions(context.Actions(), app::EditActions{});
            level = project->SourceDb().RootGroup()->CreateInstance(u8"Level", scene::SceneDocument::StaticType());
            REQUIRE(level != nullptr);
            scene::Scene authored(DefaultAllocator(), u8"authored");
            const scene::EntityHandle e = authored.CreateEntity(u8"Crate");
            crate = authored.GetEntityId(e);
            REQUIRE(scene::SaveScene(authored, *level).IsOk());
        }
        ~Bed()
        {
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<SceneEditorPage> Open()
        {
            SceneEditorPage::Services services;
            services.host = &host;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<SceneEditorPage>(DefaultAllocator(), context, Move(services), *level);
        }
    };

    // The page laid out in a GUI of its own under the editor's sheet, for its rows to be pressed
    // where they show and keys sent to what has the focus.
    struct Shown
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        gui::StyleManager styles;
        explicit Shown(SceneEditorPage& page)
        {
            String sheet;
            const char8_t* const files[] = {u8"Editor/editor.css", u8"Editor/density-default.css"};
            for (const char8_t* file : files)
            {
                Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
                REQUIRE(bytes.HasValue());
                sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
                sheet += u8"\n";
            }
            styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
            root->SetSize(Float2{1200.0f, 700.0f});
            root->AddChild(page.Content().Get());
            styles.ApplyTree(*root);
            (void)root->UpdateLayout();
        }
        ~Shown() { root->RemoveAllChildren(); }
        void Click(gui::TreeView& tree, i32 item)
        {
            (void)root->UpdateLayout();
            gui::ItemRow* row = tree.RowForItem(item);
            REQUIRE(row != nullptr);
            const gui::Rect box = row->GetScreenBounds();
            const Float2 at{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
            root->GetEventDispatcher()->InjectMouseDown(at, gui::MouseButton::Left);
            root->GetEventDispatcher()->InjectMouseUp(at, gui::MouseButton::Left);
        }
        void Key(gui::KeyCode key) { (void)root->GetEventDispatcher()->InjectKeyDown(static_cast<u32>(key)); }
    };

    gui::MenuItem* ItemNamed(gui::Node& menu, StringView text)
    {
        for (usize i = 0; i < menu.ChildCount(); ++i)
        {
            if (auto* item = Cast<gui::MenuItem>(menu.GetChildAt(i)); item != nullptr && item->GetText() == text)
                return item;
            if (gui::MenuItem* found = ItemNamed(*menu.GetChildAt(i), text))
                return found;
        }
        return nullptr;
    }

    class TestClipboard final : public gui::IClipboard
    {
    public:
        String stored;
        [[nodiscard]] bool HasText() const override { return !stored.IsEmpty(); }
        [[nodiscard]] String GetText() const override { return String(stored.AsView()); }
        void SetText(StringView text) override { stored = String(text); }
    };

    // Only the wheel: what the camera's dolly reads.
    class WheelMouse final : public shell::IMouse
    {
    public:
        f32 scroll = 0.0f;
        [[nodiscard]] f32 X() const override { return 0.0f; }
        [[nodiscard]] f32 Y() const override { return 0.0f; }
        [[nodiscard]] f32 GlobalX() const override { return 0.0f; }
        [[nodiscard]] f32 GlobalY() const override { return 0.0f; }
        [[nodiscard]] f32 DeltaX() const override { return 0.0f; }
        [[nodiscard]] f32 DeltaY() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollX() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollY() const override { return scroll; }
        [[nodiscard]] bool IsButtonDown(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonPressed(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonReleased(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };

    // The Scene creator File > New runs, over a real project.
    foundation::content::Instance* CreateScene(EditorProject& project)
    {
        pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
        pipeline::RegisterSceneCreators(creators);
        const pipeline::AssetCreator* creator = creators.FindByLabel(u8"Scene");
        REQUIRE(creator != nullptr);
        const String sources = project.SourcesRoot();
        return creator->Create(nullptr, project.SourceDb().RootGroup(), sources.AsView());
    }
}

TEST_CASE("editor camera: the wheel dolly keeps the orbit pivot at any zoom, and never goes through it")
{
    EditorCamera cam;
    cam.position = Float3{0.0f, 0.0f, 5.0f};
    cam.LookAt(Float3{0.0f, 0.0f, 0.0f}); // the pivot the origin, 5 away
    WheelMouse mouse;
    mouse.scroll = 1.0f;
    for (int i = 0; i < 40; ++i)
    {
        cam.Update(nullptr, &mouse, 1.0f / 60.0f);
        const Float3 pivot = cam.position + cam.Forward() * cam.focusDistance;
        CHECK(Length(pivot) == doctest::Approx(0.0f).epsilon(0.001f));
        CHECK(cam.position.z > 0.0f); // still on the near side
    }
    CHECK(cam.focusDistance < 1.0f);   // closer than a fixed floor of 1 would allow
    CHECK(cam.focusDistance >= 0.05f); // but never through the pivot

    // Out again along the same axis, the pivot where it was.
    mouse.scroll = -1.0f;
    for (int i = 0; i < 40; ++i)
        cam.Update(nullptr, &mouse, 1.0f / 60.0f);
    const Float3 pivot = cam.position + cam.Forward() * cam.focusDistance;
    CHECK(Length(pivot) == doctest::Approx(0.0f).epsilon(0.001f));
    CHECK(cam.focusDistance > 4.0f);
}

TEST_CASE("scene creator: each scene a SceneDocument named uniquely in Scenes, seeded with a shadow-casting Sun")
{
    scene::RegisterSceneResourceTypes();
    engine::render::RegisterRenderComponentReflection();
    const StringView dir = u8"scratch_scene_parity_creator";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));

        CHECK(pipeline::CreateSceneInstance(nullptr, u8"Scene", DefaultAllocator()) == nullptr); // no group

        foundation::content::Instance* first = CreateScene(*project);
        REQUIRE(first != nullptr);
        CHECK(first->Name() == StringView(u8"Scene"));
        CHECK(first->Path() == String(u8"Scenes/Scene"));
        CHECK(first->TypeName() == StringView(u8"SceneDocument"));
        {
            RefPtr<ISerializable> object = first->ReadObject();
            auto* doc = Cast<scene::SceneDocument>(object.Get());
            REQUIRE(doc != nullptr);
            CHECK(doc->name == String(u8"Scene"));
        }
        foundation::content::Instance* second = CreateScene(*project);
        REQUIRE(second != nullptr);
        CHECK(second->Name() == StringView(u8"Scene.2"));
        CHECK(second->Id() != first->Id());

        // Lit out of the box: an authored Sun, 40 degrees down toward +X and +Z, strong enough
        // that a first mesh's shadow reads.
        scene::Scene loaded(DefaultAllocator(), u8"loaded");
        auto* lights = loaded.AddSystem<engine::render::LightComponentManager>();
        REQUIRE(scene::LoadScene(*first, loaded).IsOk());
        scene::EntityHandle sun = scene::EntityHandle::Invalid();
        loaded.ForEachEntity([&](scene::EntityHandle e)
                             {
                                 if (loaded.GetEntityName(e) == StringView(u8"Sun"))
                                     sun = e;
                             });
        REQUIRE(sun.IsAssigned());
        engine::render::LightComponent* light = lights->Get(sun);
        REQUIRE(light != nullptr);
        CHECK(light->type == engine::render::LightType::Directional);
        CHECK(light->castsShadows);
        CHECK(light->intensity == doctest::Approx(4.0f));
        const Float3 forward = RotateVector(loaded.GetLocalTransform(sun).rotation, Float3{0, 0, -1});
        CHECK(forward.y == doctest::Approx(-0.6428f).epsilon(0.01)); // sin 40 degrees
        CHECK(forward.x > 0.5f);
        CHECK(forward.z > 0.5f);
    }
    (void)RemoveDirectoryRecursive(dir);
}

TEST_CASE("editor context: a copy the user asks for says what it copied, or that it could not")
{
    EditorContext editor{DefaultAllocator()};
    String notice;
    NoticeKind kind = NoticeKind::Info;
    editor.OnNotice = [&](NoticeKind k, StringView message)
    {
        kind = k;
        notice = String(message);
    };
    TestClipboard clipboard;
    CHECK(editor.CopyText(&clipboard, u8"Content/Hero.xasset", u8"path"));
    CHECK(clipboard.stored == String(u8"Content/Hero.xasset"));
    CHECK(kind == NoticeKind::Success);
    CHECK(notice == String(u8"Copied path"));

    // No clipboard (a headless view): a warning, not a silent no-op.
    CHECK_FALSE(editor.CopyText(nullptr, u8"x", u8"GUID"));
    CHECK(kind == NoticeKind::Warning);
    CHECK(notice == String(u8"Could not copy GUID to the clipboard"));

    // The typed editor clipboard: the slot holds the blob, and the toast names it.
    Array<byte> blob;
    blob.PushBack(byte{7});
    editor.CopyToEditorClipboard(u8"component", Move(blob), u8"component 'Light'");
    CHECK(editor.ClipboardKind() == StringView(u8"component"));
    CHECK(editor.ClipboardData(u8"component").Size() == 1u);
    CHECK(editor.ClipboardData(u8"entities").IsEmpty()); // another kind reads nothing
    CHECK(kind == NoticeKind::Success);
    CHECK(notice == String(u8"Copied component 'Light'"));
}

TEST_CASE("hierarchy: a row's Copy ID puts the entity's guid on the clipboard and says so; with no clipboard it warns")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open();
    SceneHierarchyView* tree = page->Hierarchy();
    REQUIRE(tree != nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    String notice;
    NoticeKind kind = NoticeKind::Info;
    bed.context.OnNotice = [&](NoticeKind k, StringView message)
    {
        kind = k;
        notice = String(message);
    };
    Shown shown(*page);
    TestClipboard clipboard;
    shown.root->GetEventDispatcher()->SetClipboard(&clipboard);

    // The scene's row has no id to copy.
    tree->ShowMenu(0, Float2{10.0f, 10.0f});
    REQUIRE(tree->OpenMenu() != nullptr);
    CHECK(ItemNamed(*tree->OpenMenu(), u8"Copy ID") == nullptr);
    tree->OpenMenu()->Close();

    tree->ShowMenu(tree->ItemOf(bed.crate), Float2{10.0f, 10.0f});
    REQUIRE(tree->OpenMenu() != nullptr);
    gui::MenuItem* copy = ItemNamed(*tree->OpenMenu(), u8"Copy ID");
    REQUIRE(copy != nullptr);
    CHECK(copy->HandleActivate());
    CHECK(clipboard.stored == Format(u8"{}", bed.crate)); // the guid as the scene file spells it
    CHECK(clipboard.stored.Size() == 36u);
    CHECK(kind == NoticeKind::Success);
    CHECK(notice == String(u8"Copied entity ID"));

    shown.root->GetEventDispatcher()->SetClipboard(nullptr);
    tree->ShowMenu(tree->ItemOf(bed.crate), Float2{10.0f, 10.0f});
    REQUIRE(tree->OpenMenu() != nullptr);
    copy = ItemNamed(*tree->OpenMenu(), u8"Copy ID");
    REQUIRE(copy != nullptr);
    (void)copy->HandleActivate();
    CHECK(kind == NoticeKind::Warning);
    CHECK(notice == String(u8"Could not copy entity ID to the clipboard"));
    page->OnClose();
}

TEST_CASE("hierarchy: a double-click on a row frames its entity, eased, and starts no rename; one click frames nothing")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open();
    SceneHierarchyView* tree = page->Hierarchy();
    REQUIRE(tree != nullptr);
    const Guid lantern = page->EditContext().CreateEntity(u8"Lantern");
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    EditorCamera* camera = page->ViewportCamera();
    REQUIRE(camera != nullptr);
    Shown shown(*page);

    // The scene's row: no entity, nothing framed.
    shown.Click(*tree->Tree(), 0);
    shown.Click(*tree->Tree(), 0);
    CHECK_FALSE(camera->IsGliding());

    const i32 item = tree->ItemOf(lantern);
    REQUIRE(item >= 0);
    shown.Click(*tree->Tree(), item);
    CHECK(page->EditContext().EntitySelection().Contains(lantern));
    CHECK_FALSE(camera->IsGliding()); // one click selects
    shown.Click(*tree->Tree(), item);
    CHECK(camera->IsGliding()); // the second frames, eased
    CHECK_FALSE(tree->Tree()->IsEditing());

    // Once the glide is done, the pivot is on the entity (at the origin).
    camera->Advance(1.0f);
    const Float3 pivot = camera->position + camera->Forward() * camera->focusDistance;
    CHECK(Length(pivot) == doctest::Approx(0.0f).epsilon(0.01f));
    page->OnClose();
}

TEST_CASE("hierarchy: the arrow keys move the tree's selection and the scene's follows; up onto the scene's row clears it")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open();
    SceneHierarchyView* tree = page->Hierarchy();
    REQUIRE(tree != nullptr);
    const Guid barrel = page->EditContext().CreateEntity(u8"Barrel");
    const Guid lantern = page->EditContext().CreateEntity(u8"Lantern");
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    REQUIRE(tree->Rows() == String(u8"Level |   Crate |   Barrel |   Lantern"));
    Shown shown(*page);

    shown.Click(*tree->Tree(), tree->ItemOf(bed.crate)); // the tree has the keys now
    REQUIRE(page->EditContext().EntitySelection().Contains(bed.crate));
    shown.Key(gui::KeyCode::Down);
    CHECK(page->EditContext().EntitySelection().Contains(barrel));
    CHECK(page->EditContext().EntitySelection().Items().Size() == 1u);
    shown.Key(gui::KeyCode::Down);
    CHECK(page->EditContext().EntitySelection().Contains(lantern));
    shown.Key(gui::KeyCode::Up);
    shown.Key(gui::KeyCode::Up);
    CHECK(page->EditContext().EntitySelection().Contains(bed.crate));
    shown.Key(gui::KeyCode::Up);
    CHECK(page->EditContext().EntitySelection().IsEmpty()); // the scene's row: no entity
    CHECK(tree->SceneSelected());
    page->OnClose();
}

TEST_CASE("scene view settings: a v3 view pref reads through the legacy gate with the v4 defaults; a v4 one carries the new keys")
{
    RegisterSceneViewSettingsType();
    const TypeInfo& type = SceneViewSettings::StaticType();
    CHECK(type.dataVersion == 4u);
    CHECK(type.minReadDataVersion == 3u);

    // A v3-shaped record: under a version-3 scope the body writes only the v3 keys.
    MemoryStream v3;
    {
        BinarySerializer ar(v3, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, 3u}};
        ar.PushVersionScope(chain, 1);
        SceneViewPref pref{Guid{1, 2}, false, true, true, true, true};
        pref.hasCamera = true; // must not reach the v3 record
        pref.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    (void)v3.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(v3, SerializeMode::Read);
        const SerializedDataVersion chain[] = {{type.id, 3u}};
        ar.PushVersionScope(chain, 1);
        SceneViewPref pref;
        pref.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
        CHECK(pref.scene == Guid{1, 2});
        CHECK_FALSE(pref.showGrid);
        CHECK(pref.showLodOverlay);
        CHECK(pref.showColliders);
        CHECK_FALSE(pref.showMarkers); // the v4 defaults, not the writer's values
        CHECK_FALSE(pref.showFps);
        CHECK_FALSE(pref.hasCamera);
        CHECK(pref.selection.IsEmpty());
    }

    // The same body under the current scope round-trips every key.
    MemoryStream v4;
    {
        BinarySerializer ar(v4, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, 4u}};
        ar.PushVersionScope(chain, 1);
        SceneViewPref pref{Guid{1, 2}, false, true, true, true, true};
        pref.hasCamera = true;
        pref.cameraYaw = 0.5f;
        pref.selection.PushBack(Guid{9, 9});
        pref.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    (void)v4.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(v4, SerializeMode::Read);
        const SerializedDataVersion chain[] = {{type.id, 4u}};
        ar.PushVersionScope(chain, 1);
        SceneViewPref pref;
        pref.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
        CHECK(pref.showMarkers);
        CHECK(pref.showFps);
        CHECK(pref.hasCamera);
        CHECK(pref.cameraYaw == doctest::Approx(0.5f));
        REQUIRE(pref.selection.Size() == 1u);
        CHECK(pref.selection[0] == Guid{9, 9});
    }
}

TEST_CASE("scene view settings: the frame-rate readout says the window's rate and mean frame time")
{
    CHECK(FrameRateOverlayText(0.0, 0) == String(u8"-- fps"));
    CHECK(FrameRateOverlayText(0.5, 0) == String(u8"-- fps"));
    CHECK(FrameRateOverlayText(0.5, 30) == String(u8"60 fps  16.7 ms"));
    CHECK(FrameRateOverlayText(1.0, 144) == String(u8"144 fps  6.9 ms"));
    CHECK(FrameRateOverlayText(0.5, 12) == String(u8"24 fps  41.7 ms"));
}

namespace
{
    const TypeInfo* EnvType() { return &TypeOf<engine::render::EnvironmentSettings>(); }

    // A scene whose environment uses `profile` (a loaded product under `id`).
    struct ProfiledScene
    {
        scene::Scene scene{DefaultAllocator(), u8"Level"};
        engine::render::EnvironmentSystem* env = nullptr;
        RefPtr<engine::render::EnvironmentProfile> profile = MakeRef<engine::render::EnvironmentProfile>(DefaultAllocator());
        EditorCommandStack commands;
        SceneEditContext edit{scene, commands};
        Guid id{0x51u, 0x7Eu};

        ProfiledScene()
        {
            engine::render::RegisterRenderComponentReflection();
            env = scene.AddSystem<engine::render::EnvironmentSystem>();
            env->Environment().ambientIntensity = 0.25f;
            profile->values.ambientIntensity = 0.9f;
            env->Environment().source = engine::render::SettingsSource::Profile;
            env->Environment().profile = profile.Get();
            env->Environment().profile.SetId(id);
        }
    };
}

TEST_CASE("settings profiles: a value edit lands in the profile, the block's own fields in the scene; each undoes")
{
    using engine::render::SettingsSource;
    ProfiledScene s;
    Array<Guid> edited;
    s.edit.OnSettingsProfileEdited = [&edited](const TypeInfo*, const Guid& profile) { edited.PushBack(profile); };

    s.edit.SetSceneSettingProperty(EnvType(), "ambientIntensity", Variant::From<f32>(0.7f));
    CHECK(s.profile->values.ambientIntensity == doctest::Approx(0.7f));
    CHECK(s.env->Environment().ambientIntensity == doctest::Approx(0.25f));
    REQUIRE(edited.Size() == 1);
    CHECK(edited[0] == s.id);

    // An enum, written raw, goes the same way.
    s.edit.SetSceneSettingPropertyRaw(EnvType(), "skyMode", static_cast<i64>(foundation::render::SkyMode::Analytic));
    CHECK(s.profile->values.skyMode == foundation::render::SkyMode::Analytic);

    s.commands.Undo();
    s.commands.Undo();
    CHECK(s.profile->values.ambientIntensity == doctest::Approx(0.9f));
    CHECK(s.profile->values.skyMode != foundation::render::SkyMode::Analytic);
    CHECK(edited.Size() == 4); // every write to the profile is persisted, the undos too

    // The source is the scene's own ("sceneOnly"): switching it edits the block, and after it an
    // edit lands in the scene's own values.
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
    using engine::render::SettingsSource;
    ProfiledScene s;
    s.profile->values.turbidity = 6.0f;

    REQUIRE(s.edit.MutateSceneSettings(EnvType(), [](scene::SceneSystem& system) { system.CopySettingsProfileIntoScene(); }));
    CHECK(s.env->Environment().source == SettingsSource::Scene);
    CHECK(s.env->Environment().ambientIntensity == doctest::Approx(0.9f));
    CHECK(s.env->Environment().turbidity == doctest::Approx(6.0f));
    CHECK(s.env->Environment().profile.id == s.id); // kept: the profile is a pick away
    s.commands.Undo();
    CHECK(s.env->Environment().source == SettingsSource::Profile);
    CHECK(s.env->Environment().ambientIntensity == doctest::Approx(0.25f));

    const Guid other{0x99u, 0x1u};
    REQUIRE(s.edit.MutateSceneSettings(EnvType(), [other](scene::SceneSystem& system) { system.UseSettingsProfile(other); }));
    CHECK(s.env->Environment().profile.id == other);
    s.commands.Undo();
    CHECK(s.env->Environment().profile.id == s.id);
}

TEST_CASE("settings profiles: Make Profile writes the values and switches; an edit in profile mode reaches the asset on save")
{
    using engine::render::SettingsSource;
    pipeline::RegisterRenderProfileAssets();
    engine::render::RegisterRenderComponentReflection();
    const StringView dir = u8"scratch_scene_parity_profiles";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    {
        EditorContext context(DefaultAllocator());
        context.SetProject(project.Get());
        pipeline::RegisterRenderCreators(context.Creators());
        // The join the app wires from its builders: the environment profile's asset.
        context.SourceAssetTypesOf = [](const TypeInfo& product)
        {
            Array<const TypeInfo*> out;
            if (&product == &engine::render::EnvironmentProfile::StaticType())
                out.PushBack(&pipeline::EnvironmentProfileAsset::StaticType());
            return out;
        };

        scene::Scene level(DefaultAllocator(), u8"Level");
        auto* env = level.AddSystem<engine::render::EnvironmentSystem>();
        env->Environment().ambientIntensity = 0.3f;
        env->Environment().turbidity = 5.0f;
        EditorCommandStack commands;
        SceneEditContext edit(level, commands);

        foundation::content::Instance* made = MakeSettingsProfile(context, edit, EnvType(), u8"Level Environment");
        REQUIRE(made != nullptr);
        CHECK(made->Path() == String(u8"Profiles/Level Environment"));
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
        commands.Undo(); // the switch is one step
        CHECK(env->Environment().source == SettingsSource::Scene);
        commands.Redo();

        // The profile loaded (here by hand; the editor binds the cooked product): an edit in
        // profile mode is queued and the save flow writes it to the asset.
        RefPtr<engine::render::EnvironmentProfile> product = MakeRef<engine::render::EnvironmentProfile>(DefaultAllocator());
        product->values = env->Environment();
        env->Environment().profile = product.Get();
        env->Environment().profile.SetId(id);
        edit.OnSettingsProfileEdited = [&](const TypeInfo* type, const Guid& profile)
        {
            if (scene::SceneSystem* system = edit.FindSystemBySettingsType(type))
                QueueSettingsProfileEdit(context, *system, profile);
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

TEST_CASE("settings profiles: the settings source reads a value where the block's values are now, and writes it there")
{
    using engine::render::SettingsSource;
    ProfiledScene s;
    s.env->Environment().turbidity = 3.0f;
    s.profile->values.turbidity = 9.0f;
    Array<Guid> edited;
    s.edit.OnSettingsProfileEdited = [&edited](const TypeInfo*, const Guid& profile) { edited.PushBack(profile); };
    app::XmlPropertyTextFormat format(DefaultAllocator());
    SceneSettingsSource source(DefaultAllocator(), s.edit, *EnvType(), format);

    // Using the profile: a value field reads the profile's; the source (scene only) the block's.
    CHECK(source.GetValue(u8"turbidity", 0).AsFloat() == doctest::Approx(9.0));
    CHECK(source.GetValue(u8"source", 0).AsInt() == static_cast<i64>(SettingsSource::Profile));
    gui::PropertyWrite write;
    write.Value = gui::Variant(4.0);
    source.BeginEdit(u8"turbidity");
    source.CommitEdit(u8"turbidity", write);
    CHECK(s.profile->values.turbidity == doctest::Approx(4.0f));
    CHECK(s.env->Environment().turbidity == doctest::Approx(3.0f));
    REQUIRE(edited.Size() == 1);
    CHECK(edited[0] == s.id);

    // The source switched: the next refresh follows the block's own values.
    s.edit.SetSceneSettingPropertyRaw(EnvType(), "source", static_cast<i64>(SettingsSource::Scene));
    source.Refresh();
    CHECK(source.GetValue(u8"turbidity", 0).AsFloat() == doctest::Approx(3.0));
    source.BeginEdit(u8"turbidity");
    source.CommitEdit(u8"turbidity", write);
    CHECK(s.env->Environment().turbidity == doctest::Approx(4.0f));
    CHECK(s.profile->values.turbidity == doctest::Approx(4.0f)); // the profile's own edit only
    CHECK(edited.Size() == 1);
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

TEST_CASE("scene loading: a scene's world bounds merge every measured entity; nothing measured is no bounds")
{
    scene::Scene level(DefaultAllocator(), u8"level");
    AABB bounds;
    CHECK_FALSE(SceneWorldBounds(level, bounds)); // nothing has a size
    auto* boxes = level.AddSystem<TwoBoxes>();
    boxes->a = level.CreateEntity(u8"A");
    boxes->b = level.CreateEntity(u8"B");
    (void)level.CreateEntity(u8"Marker"); // measured by nothing
    REQUIRE(SceneWorldBounds(level, bounds));
    CHECK(bounds.min.x == doctest::Approx(0.0f));
    CHECK(bounds.min.y == doctest::Approx(-2.0f));
    CHECK(bounds.max.x == doctest::Approx(5.0f));
    CHECK(bounds.max.z == doctest::Approx(3.0f));
}

TEST_CASE("inspector: a scene settings section is titled with its type's display name, else its name less Settings")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open();
    SceneInspectorView* inspector = page->Inspector();
    REQUIRE(inspector != nullptr);
    Shown shown(*page);
    shown.Click(*page->Hierarchy()->Tree(), 0); // the scene's row
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    REQUIRE(inspector->Kind() == StringView(u8"Scene"));
    SceneEditContext& edit = page->EditContext();
    REQUIRE(edit.FindSystemBySettingsType(&TypeOf<engine::render::EnvironmentSettings>()) != nullptr);
    CHECK(inspector->SettingsOf(u8"Environment") != nullptr);
    REQUIRE(edit.FindSystemBySettingsType(&TypeOf<engine::render::PostProcessSettings>()) != nullptr);
    CHECK(inspector->SettingsOf(u8"Post Processing") != nullptr);
    REQUIRE(edit.FindSystemBySettingsType(&TypeOf<engine::physics::PhysicsSceneSettings>()) != nullptr);
    CHECK(inspector->SettingsOf(u8"Physics") != nullptr);
    REQUIRE(edit.FindSystemBySettingsType(&TypeOf<engine::navigation::NavigationSceneSettings>()) != nullptr);
    CHECK(inspector->SettingsOf(u8"Navigation") != nullptr);
    REQUIRE(edit.FindSystemBySettingsType(&TypeOf<engine::script::SceneScriptSettings>()) != nullptr);
    CHECK(inspector->SettingsOf(u8"Scene Script") != nullptr);
    // No C++ name reads through.
    const String titles = inspector->SectionTitles();
    CHECK_FALSE(titles.AsView().ContainsIgnoreCase(u8"PostProcess"));
    CHECK_FALSE(titles.AsView().ContainsIgnoreCase(u8"PhysicsScene"));
    CHECK_FALSE(titles.AsView().ContainsIgnoreCase(u8"Settings"));
    page->OnClose();
}

namespace
{
    // The visibleWhen attribute's text on `type`'s property `name`.
    StringView VisibleWhen(const TypeInfo& type, const char* name)
    {
        const PropertyInfo* property = FindProperty(type, name);
        REQUIRE(property != nullptr);
        const Attribute* visible = FindAttribute(*property, u8"visibleWhen");
        REQUIRE(visible != nullptr);
        const String* text = visible->value.TryGet<String>();
        REQUIRE(text != nullptr);
        return text->AsView();
    }

    // Whether the row `name` of the reflected source over `object` shows now.
    bool RowVisible(const gui::ReflectedPropertySource& source, StringView name)
    {
        Array<gui::PropertyInfo> rows;
        source.GetProperties(StringView{}, rows);
        for (const gui::PropertyInfo& row : rows)
            if (row.Name.AsView() == name)
                return row.Visible;
        FAIL_CHECK("no such row");
        return false;
    }
}

TEST_CASE("inspector attributes: the render components carry visibleWhen, range and displayName, and the reflected source shows and hides by them")
{
    engine::render::RegisterRenderComponentReflection();
    using engine::render::CameraComponent;
    using engine::render::CameraProjection;
    using engine::render::EnvironmentSettings;
    using engine::render::LightComponent;
    using engine::render::LightType;

    // Light: spot-only angles behind the type; the shadow controls while it casts shadows;
    // intensity a slider.
    const TypeInfo& light = TypeOf<LightComponent>();
    CHECK(VisibleWhen(light, "innerAngle") == StringView(u8"type=2"));
    CHECK(VisibleWhen(light, "outerAngle") == StringView(u8"type=2"));
    const PropertyInfo* intensity = FindProperty(light, "intensity");
    REQUIRE(intensity != nullptr);
    const Attribute* range = FindAttribute(*intensity, u8"range");
    REQUIRE(range != nullptr);
    CHECK(range->value.TryGet<Float4>() != nullptr);
    for (const char* name : {"shadowStrength", "shadowNormalBias", "shadowDepthBiasScale"})
    {
        CHECK(VisibleWhen(light, name) == StringView(u8"castsShadows"));
        const PropertyInfo* control = FindProperty(light, name);
        REQUIRE(control != nullptr);
        CHECK(FindAttribute(*control, u8"range") != nullptr);
    }
    {
        LightComponent lamp;
        lamp.type = LightType::Directional;
        lamp.castsShadows = false;
        gui::ReflectedPropertySource source(DefaultAllocator());
        const Instance object(&lamp, &light);
        source.SetObjects(Span<const Instance>(&object, 1u));
        CHECK_FALSE(RowVisible(source, u8"innerAngle"));
        CHECK_FALSE(RowVisible(source, u8"shadowStrength"));
        lamp.type = LightType::Spot;
        lamp.castsShadows = true;
        CHECK(RowVisible(source, u8"innerAngle"));
        CHECK(RowVisible(source, u8"shadowStrength"));
        lamp.type = LightType::Point;
        CHECK_FALSE(RowVisible(source, u8"innerAngle"));
        CHECK(RowVisible(source, u8"range")); // type=1,2
    }

    // Camera: the field of view for a perspective camera, the height for an orthographic one, the
    // mode an enum by name.
    const TypeInfo& camera = TypeOf<CameraComponent>();
    CHECK(VisibleWhen(camera, "fovYRadians") == StringView(u8"projection=0"));
    CHECK(VisibleWhen(camera, "orthoHeight") == StringView(u8"projection=1"));
    CHECK(TypeOf<CameraProjection>().enumeratorCount == 2u);
    {
        CameraComponent eye;
        eye.projection = CameraProjection::Perspective;
        gui::ReflectedPropertySource source(DefaultAllocator());
        const Instance object(&eye, &camera);
        source.SetObjects(Span<const Instance>(&object, 1u));
        CHECK(RowVisible(source, u8"fovYRadians"));
        CHECK_FALSE(RowVisible(source, u8"orthoHeight"));
        eye.projection = CameraProjection::Orthographic;
        CHECK_FALSE(RowVisible(source, u8"fovYRadians"));
        CHECK(RowVisible(source, u8"orthoHeight"));
    }

    // Environment: turbidity Analytic-only with a bounded slider; the shared zenith / color slot's
    // own name.
    const TypeInfo& env = TypeOf<EnvironmentSettings>();
    CHECK(VisibleWhen(env, "turbidity") == StringView(u8"skyMode=1"));
    const PropertyInfo* turbidity = FindProperty(env, "turbidity");
    REQUIRE(turbidity != nullptr);
    CHECK(FindAttribute(*turbidity, u8"range") != nullptr);
    const PropertyInfo* zenith = FindProperty(env, "skyZenith");
    REQUIRE(zenith != nullptr);
    const Attribute* label = FindAttribute(*zenith, u8"displayName");
    REQUIRE(label != nullptr);
    REQUIRE(label->value.TryGet<String>() != nullptr);
    CHECK(label->value.TryGet<String>()->AsView() == StringView(u8"Sky Zenith / Color"));
    {
        EnvironmentSettings settings;
        settings.skyMode = foundation::render::SkyMode::Analytic;
        gui::ReflectedPropertySource source(DefaultAllocator());
        const Instance object(&settings, &env);
        source.SetObjects(Span<const Instance>(&object, 1u));
        CHECK(RowVisible(source, u8"turbidity"));
        settings.skyMode = foundation::render::SkyMode::HDREquirect;
        CHECK_FALSE(RowVisible(source, u8"turbidity"));
        Array<gui::PropertyInfo> rows;
        source.GetProperties(StringView{}, rows);
        bool named = false;
        for (const gui::PropertyInfo& row : rows)
            named = named || (row.Name.AsView() == StringView(u8"skyZenith") && row.DisplayName.AsView() == StringView(u8"Sky Zenith / Color"));
        CHECK(named);
    }
}

