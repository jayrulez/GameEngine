// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The scene page on the GUI (Documentation/Specs/editor-gui.md, 5.3, layer 2), headless: a scratch
// project, a runtime context with a scene subsystem and no renderer, the page's own markup. It
// opens its scene into a scene of its own with the viewport in its pane; its bar's Save follows its
// own page; a scene that did not load says so on the page and is not saved over; a prefab is marked
// and refused a second root; Simulate locks the edits and gives the scene back; where the scene was
// left (the camera, the selection, the panes) comes back when it opens again. And the parts: the
// panes' weights against the prefs' nested ratios, and a prefab's own instance refused.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import foundation.settings;
import foundation.scene;
import engine.scene;
import foundation.scene.resource;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.scene;

using namespace foundation::core;
using namespace editor;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace gui = foundation::gui;

namespace
{
    // A host whose context has a scene subsystem and nothing else: no renderer, no window.
    class SceneHost final : public runtime::IApplicationHost
    {
    public:
        SceneHost()
        {
            m_context.AddSubsystem<engine::scene::SceneSubsystem>();
            m_context.Startup();
        }
        ~SceneHost() override { m_context.Shutdown(); }
        runtime::Context& Ctx() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&, const foundation::graphics::RenderWindowDesc&) override
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

    template <typename T>
    T* FindId(gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindId<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    // A project with a scene ("Level", a Crate in it), a prefab ("Barrel") and a scene whose source
    // is not a scene ("Broken").
    struct Bed
    {
        String dir = String(u8"scratch_scene_page_project");
        UniquePtr<EditorProject> project;
        foundation::settings::Settings store{DefaultAllocator()}; // the project's editor settings
        EditorContext context{DefaultAllocator()};
        SceneHost host;
        foundation::content::Instance* level = nullptr;
        foundation::content::Instance* barrel = nullptr;
        foundation::content::Instance* broken = nullptr;
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
            // The bar's Save, Undo and Redo, as the editor registers them.
            app::FileActions file;
            file.save = [](EditorPage& page) { (void)page.Save(); };
            app::RegisterFileActions(context.Actions(), Move(file));
            app::RegisterEditActions(context.Actions(), app::EditActions{});
            foundation::content::Group* top = project->SourceDb().RootGroup();
            level = top->CreateInstance(u8"Level", scene::SceneDocument::StaticType());
            barrel = top->CreateInstance(u8"Barrel", scene::PrefabDocument::StaticType());
            broken = top->CreateInstance(u8"Broken", scene::SceneDocument::StaticType());
            REQUIRE(level != nullptr);
            REQUIRE(barrel != nullptr);
            REQUIRE(broken != nullptr);
            {
                scene::Scene authored(DefaultAllocator(), u8"authored");
                const scene::EntityHandle e = authored.CreateEntity(u8"Crate");
                crate = authored.GetEntityId(e);
                REQUIRE(scene::SaveScene(authored, *level).IsOk());
            }
            const char8_t junk[] = u8"this is not a scene";
            REQUIRE(broken->WriteData(u8"scene", Span<const byte>(reinterpret_cast<const byte*>(junk), sizeof(junk) - 1),
                                      foundation::content::StreamEncoding::Text)
                        .IsOk());
        }
        ~Bed()
        {
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<SceneEditorPage> Open(foundation::content::Instance& instance)
        {
            SceneEditorPage::Services services;
            services.host = &host;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<SceneEditorPage>(DefaultAllocator(), context, Move(services), instance);
        }
    };
}

TEST_CASE("scene page: its scene opened into a scene of its own, the viewport in its pane, its bar's Save following its own page")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    REQUIRE(page->ScenePtr() != nullptr);
    CHECK(page->ScenePtr()->FindEntity(bed.crate).IsAssigned());
    CHECK_FALSE(page->LoadFailed());
    CHECK_FALSE(page->IsPrefab());
    RefPtr<gui::Node> content = page->Content();
    REQUIRE(content);
    gui::Node* pane = FindId<gui::Node>(*content, u8"scene-viewport");
    REQUIRE(pane != nullptr);
    CHECK(pane->GetChildAt(0) == static_cast<gui::Node*>(&page->Viewport().View()));
    CHECK_FALSE(FindId<gui::Node>(*content, u8"scene-state")->IsDisplayed());
    CHECK_FALSE(FindId<gui::Node>(*content, u8"scene-kind")->IsDisplayed());

    // Clean: Save is off; an edit makes it dirty and the bar follows on its next frame.
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    auto* save = FindId<gui::ToolButton>(*content, u8"scene-save");
    REQUIRE(save != nullptr);
    CHECK(save->GetTooltipText().StartsWith(u8"Save"));
    CHECK_FALSE(save->IsEnabled());
    (void)page->EditContext().CreateEntity(u8"Barrel");
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(page->IsDirty());
    CHECK(save->IsEnabled());
    CHECK(page->Save().IsOk());
    CHECK_FALSE(page->IsDirty());
    page->OnClose();
}

TEST_CASE("scene page: a scene that did not load says so on the page and is not saved over")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.broken);
    CHECK(page->LoadFailed());
    RefPtr<gui::Node> content = page->Content();
    CHECK(FindId<gui::Node>(*content, u8"scene-state")->IsDisplayed());
    (void)page->EditContext().CreateEntity(u8"Lost");
    CHECK(page->Save().Code() == ErrorCode::NotSupported);
    // The source is as it was.
    UniquePtr<IStream> stream = bed.broken->ReadData(u8"scene");
    REQUIRE(stream.Get() != nullptr);
    CHECK(stream->Size() == static_cast<u64>(sizeof(u8"this is not a scene") - 1));
    page->OnClose();
}

TEST_CASE("scene page: a prefab is marked as one and refused a second root")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.barrel);
    CHECK(page->IsPrefab());
    CHECK(FindId<gui::Node>(*page->Content(), u8"scene-kind")->IsDisplayed());
    (void)page->EditContext().CreateEntity(u8"Hoop");
    CHECK(page->Save().IsOk()); // one root
    (void)page->EditContext().CreateEntity(u8"Lid");
    CHECK(page->Save().Code() == ErrorCode::InvalidArgument);
    CHECK(PrefabFlows::SaveRefusal(*page->ScenePtr(), bed.barrel->Id()).AsView().StartsWith(u8"A prefab needs exactly one root"));
    page->OnClose();
}

TEST_CASE("scene page: Simulate locks the edits and gives the scene back as it was")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    const usize before = page->ScenePtr()->EntityCount();
    page->StartSimulation();
    CHECK(page->IsSimulating());
    CHECK(page->EditContext().CreateEntity(u8"Refused").IsNil()); // the stack is locked
    (void)page->ScenePtr()->CreateEntity(u8"Spawned by the run");
    page->PauseSimulation(true);
    CHECK(page->IsPaused());
    page->StopSimulation();
    CHECK_FALSE(page->IsSimulating());
    CHECK(page->ScenePtr()->EntityCount() == before);
    CHECK(page->ScenePtr()->FindEntity(bed.crate).IsAssigned());
    CHECK_FALSE(page->EditContext().CreateEntity(u8"Allowed").IsNil());
    page->OnClose();
}

TEST_CASE("scene page: where the scene was left (the camera, the selection, the panes, the overlays) comes back when it opens again")
{
    Bed bed;
    {
        UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
        page->Viewport().Camera().position = Float3{4.0f, 5.0f, 6.0f};
        page->EditContext().EntitySelection().Set(Span<const Guid>(&bed.crate, 1u));
        page->Overlays().showFps = true;
        auto* body = FindId<gui::Splitter>(*page->Content(), u8"scene-body");
        REQUIRE(body != nullptr);
        const f32 weights[3] = {0.3f, 0.5f, 0.2f};
        body->SetWeights(Span<const f32>(weights, 3u));
        page->OnClose();
    }
    UniquePtr<SceneEditorPage> again = bed.Open(*bed.level);
    CHECK(again->Viewport().Camera().position.x == doctest::Approx(4.0f));
    CHECK(again->EditContext().EntitySelection().Contains(bed.crate));
    CHECK(again->Overlays().showFps);
    const Array<f32> weights = FindId<gui::Splitter>(*again->Content(), u8"scene-body")->GetWeights();
    REQUIRE(weights.Size() == 3u);
    const f32 total = weights[0] + weights[1] + weights[2];
    CHECK(weights[0] / total == doctest::Approx(0.3f));
    CHECK(weights[2] / total == doctest::Approx(0.2f));
    again->OnClose();
}

TEST_CASE("scene view state: the panes' weights and the prefs' nested ratios convert both ways")
{
    const ScenePaneWeights weights = SceneViewState::WeightsFromSplits(0.2f, 0.72f);
    CHECK(weights.hierarchy == doctest::Approx(0.2f));
    CHECK(weights.viewport == doctest::Approx(0.8f * 0.72f));
    CHECK(weights.inspector == doctest::Approx(0.8f * 0.28f));
    f32 hierarchy = 0.0f;
    f32 inspector = 0.0f;
    SceneViewState::SplitsFromWeights(ScenePaneWeights{2.0f, 6.0f, 2.0f}, hierarchy, inspector); // any scale
    CHECK(hierarchy == doctest::Approx(0.2f));
    CHECK(inspector == doctest::Approx(0.75f));
}

TEST_CASE("prefab flows: a prefab's page refuses an instance of itself, saying so")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.barrel);
    PrefabFlows::Services services;
    services.context = &bed.context;
    services.edit = &page->EditContext();
    const Guid self = bed.barrel->Id();
    services.pageAsset = [self]() { return self; };
    PrefabFlows flows(Move(services));
    CHECK_FALSE(flows.Spawn(self, Guid{}));
    CHECK_FALSE(flows.Spawn(Guid{}, Guid{})); // nothing picked
    page->OnClose();
}
