// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The scene page on the GUI (Documentation/Specs/editor-gui.md, 5.3, layer 2), headless: a scratch
// project, a runtime context with a scene subsystem and no renderer, the page's own markup. It
// opens its scene into a scene of its own with the viewport in its pane; its bar's Save follows its
// own page; a scene that did not load says so on the page and is not saved over; a prefab is marked
// and refused a second root; Simulate locks the edits and gives the scene back; where the scene was
// left (the camera, the selection, the panes) comes back when it opens again; the animation panel
// under the viewport (shown and hidden, an animator's clip opened in it, its clip asked about apart
// from the scene, the scene saved as it is, not as the panel posed it). And the parts: the
// panes' weights against the prefs' nested ratios, and a prefab's own instance refused.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.resource;
import foundation.geometry;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import foundation.settings;
import foundation.scene;
import engine.scene;
import foundation.render;    // debug::DebugDraw (the overlays)
import engine.render;      // RegisterRenderComponentReflection, LightComponent
import engine.composition; // FullSceneComposition
import engine.physics;
import engine.navigation;
import engine.animation;
import foundation.animation; // Skeleton (a bone name's choices)
import foundation.physics;
import engine.script;
import foundation.script.resource;
import foundation.scene.resource;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.scene;
import editor.viewporttools;
import foundation.propertyanimation;
import propertyanimation.pipeline;
import editor.propertyanimation;

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
            // The engine's full composition, so a scene's entities take its components.
            engine::RegisterAllSceneComponentReflection(); // every component named, as the editor has them
            m_context.AddSubsystem<engine::scene::SceneSubsystem>()->SetComposition(engine::FullSceneComposition());
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
        app::PickerRequest picked; // what the page last asked its picker to show
        UniquePtr<SceneEditorPage> Open(foundation::content::Instance& instance)
        {
            SceneEditorPage::Services services;
            services.host = &host;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            services.pick = [this](app::PickerRequest request) { picked = Move(request); };
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

TEST_CASE("scene page: the animation panel under the viewport, its clip its own, the scene saved as it is and not as posed")
{
    Bed bed;
    pipeline::RegisterPropertyAnimationAssets();
    foundation::content::Instance* clip = pipeline::CreatePropertyAnimationClip(bed.project->SourceDb().RootGroup(), u8"Bounce");
    REQUIRE(clip != nullptr);
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    PropertyAnimationPanel* panel = page->AnimationPanel();
    REQUIRE(panel != nullptr);
    gui::Splitter* center = FindId<gui::Splitter>(*page->Content(), u8"scene-center");
    REQUIRE(center != nullptr);
    CHECK(center->GetPaneCount() == 1u); // hidden: the viewport takes it all
    CHECK_FALSE(page->AnimationPanelShown());
    page->SetAnimationPanelShown(true);
    CHECK(center->GetPaneCount() == 2u);
    page->SetAnimationPanelShown(false);
    CHECK(center->GetPaneCount() == 1u);

    // An animator's Edit Clip: shown, the clip opened, the entity bound.
    page->EditAnimationClip(clip->Id(), bed.crate);
    CHECK(page->AnimationPanelShown());
    CHECK(panel->Document().ClipId() == clip->Id());
    CHECK(panel->BoundEntity() == bed.crate);

    // A track posing the crate at x 5: the clip unsaved, never the scene.
    AnimatablePropertyInfo position;
    for (const AnimatablePropertyInfo& seed : panel->OfferedTracks())
        if (seed.propertyPath == String(u8"position"))
            position = seed;
    REQUIRE_FALSE(position.propertyPath.IsEmpty());
    panel->AddTrack(position);
    panel->Document().Edit(u8"test", [](foundation::propertyanimation::PropertyAnimationClip& c) { c.tracks[0].channels[0].Keys()[0].value = 5.0f; });
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    const scene::EntityHandle crate = page->ScenePtr()->FindEntity(bed.crate);
    CHECK(page->ScenePtr()->GetLocalTransform(crate).position.x == doctest::Approx(5.0f));
    CHECK_FALSE(page->IsDirty());
    Array<String> besides;
    page->ListUnsavedBesides(besides);
    REQUIRE(besides.Size() == 1u);
    CHECK(besides[0] == String(u8"The clip 'Bounce'"));
    CHECK(page->HasUnsavedWork());

    // Saved: the scene as it is (x 0), the crate posed again after.
    (void)page->EditContext().CreateEntity(u8"Barrel");
    REQUIRE(page->Save().IsOk());
    CHECK(page->ScenePtr()->GetLocalTransform(crate).position.x == doctest::Approx(5.0f));
    {
        scene::Scene stored(DefaultAllocator(), u8"stored");
        REQUIRE(scene::LoadScene(*bed.level, stored).IsOk());
        CHECK(stored.GetLocalTransform(stored.FindEntity(bed.crate)).position.x == doctest::Approx(0.0f));
    }
    REQUIRE(page->SaveBesides().IsOk());
    besides.Clear();
    page->ListUnsavedBesides(besides);
    CHECK(besides.IsEmpty());

    // Hidden from its own Close: what it posed goes back.
    REQUIRE(panel->RequestClose);
    panel->RequestClose();
    CHECK_FALSE(page->AnimationPanelShown());
    CHECK(page->ScenePtr()->GetLocalTransform(crate).position.x == doctest::Approx(0.0f));

    // New on an entity whose animator has no clip: the clip made in Animations and given to it,
    // one step of the scene's; an animator playing a clip keeps it.
    page->EditContext().AddComponent(bed.crate, &TypeOf<engine::animation::PropertyAnimatorComponent>());
    REQUIRE(panel->RequestNewClip);
    panel->RequestNewClip();
    auto* animators = page->ScenePtr()->GetSystem<engine::animation::PropertyAnimatorComponentManager>();
    REQUIRE(animators != nullptr);
    const Guid made = animators->Get(crate)->clip.id;
    CHECK_FALSE(made.IsNil());
    CHECK(panel->Document().ClipId() == made);
    CHECK(panel->Document().Name() == StringView(u8"Crate Clip"));
    page->Commands().Undo();
    CHECK(animators->Get(crate)->clip.id.IsNil());
    page->Commands().Redo();
    panel->RequestNewClip(); // another: the animator keeps the first
    CHECK(animators->Get(crate)->clip.id == made);
    CHECK(panel->Document().ClipId() != made);
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

TEST_CASE("scene overlays: the entity the animation panel poses is marked, a sphere and \"preview\" over it")
{
    scene::Scene scene(DefaultAllocator(), u8"overlays");
    const scene::EntityHandle e = scene.CreateEntity(u8"Door");
    Transform moved;
    moved.position = Float3{2.0f, 1.0f, 0.0f};
    scene.SetLocalTransform(e, moved);
    scene.UpdateTransforms();
    EditorCamera camera;
    SceneOverlays overlays;
    overlays.settings.showGrid = false;
    SceneOverlayFrame frame;
    frame.scene = &scene;
    frame.camera = &camera;
    foundation::render::debug::DebugDraw plain;
    overlays.Draw(plain, frame);
    CHECK(plain.TextCommands3D().IsEmpty());

    frame.previewed = scene.GetEntityId(e);
    foundation::render::debug::DebugDraw marked;
    overlays.Draw(marked, frame);
    REQUIRE(marked.TextCommands3D().Size() == 1u);
    const auto& text = marked.TextCommands3D()[0];
    CHECK(text.worldPos.x == doctest::Approx(2.0f));
    CHECK(text.worldPos.y == doctest::Approx(1.0f));
    CHECK(StringView(reinterpret_cast<const utf8char*>(marked.TextChars().Data()) + text.textStart, static_cast<usize>(text.textLength)) == StringView(u8"preview"));
    CHECK(marked.OverlayLineVertices().Size() > plain.OverlayLineVertices().Size());
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

namespace
{
    // The page laid out in a GUI of its own under the editor's sheet, for its hierarchy's rows to be
    // pressed where they show.
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
        void Click(gui::Node& node)
        {
            (void)root->UpdateLayout();
            const gui::Rect box = node.GetScreenBounds();
            const Float2 at{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
            root->GetEventDispatcher()->InjectMouseDown(at, gui::MouseButton::Left);
            root->GetEventDispatcher()->InjectMouseUp(at, gui::MouseButton::Left);
        }
        void Click(gui::TreeView& tree, i32 item, u32 modifiers = 0)
        {
            (void)root->UpdateLayout();
            gui::ItemRow* row = tree.RowForItem(item);
            REQUIRE(row != nullptr);
            const gui::Rect box = row->GetScreenBounds();
            const Float2 at{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
            root->GetEventDispatcher()->InjectMouseDown(at, gui::MouseButton::Left, modifiers);
            root->GetEventDispatcher()->InjectMouseUp(at, gui::MouseButton::Left, modifiers);
        }
    };
}

TEST_CASE("hierarchy: the scene its top row, its entities under it as they nest; a click selects, the page's selection shows in it")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneHierarchyView* tree = page->Hierarchy();
    REQUIRE(tree != nullptr);
    const Guid lid = page->EditContext().CreateEntity(u8"Lid", bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Lid")); // Crate gained a child: it opens
    const Guid knob = page->EditContext().CreateEntity(u8"Knob", lid);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Lid |       Knob"));
    tree->Tree()->CollapseItem(tree->ItemOf(lid)); // closed by the user: it stays closed
    (void)page->EditContext().CreateEntity(u8"Spring", lid);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Lid"));
    (void)knob;

    // The page's selection (a viewport pick) shows in the tree; a click on a row is the page's.
    page->EditContext().EntitySelection().Set(bed.crate);
    CHECK(tree->Tree()->IsItemSelected(tree->ItemOf(bed.crate)));
    Shown shown(*page);
    shown.Click(*tree->Tree(), tree->ItemOf(lid));
    CHECK(page->EditContext().EntitySelection().Contains(lid));
    CHECK_FALSE(page->EditContext().EntitySelection().Contains(bed.crate));
    shown.Click(*tree->Tree(), tree->ItemOf(bed.crate), gui::KeyModCtrl); // several
    CHECK(page->EditContext().EntitySelection().Items().Size() == 2u);
    shown.Click(*tree->Tree(), 0); // the scene's row: no entity
    CHECK(tree->Tree()->GetSelectionCount() == 1u);
    CHECK(tree->Tree()->IsItemSelected(0));
    CHECK(page->EditContext().EntitySelection().IsEmpty());

    // An entity that is not active dims; the model says so for its row.
    page->EditContext().SetEntityActive(lid, false);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    const gui::Variant classes = tree->Tree()->GetModel()->Data(tree->IndexOf(lid), gui::ModelRole::RowClass);
    CHECK(classes.ToString() == String(u8"inactive"));
    page->OnClose();
}

TEST_CASE("hierarchy: a search keeps a match's way; a whole guid selects that entity; a rename is one undo")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneHierarchyView* tree = page->Hierarchy();
    REQUIRE(tree != nullptr);
    const Guid lid = page->EditContext().CreateEntity(u8"Lid", bed.crate);
    (void)page->EditContext().CreateEntity(u8"Barrel");
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    tree->Search(u8"lid");
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Lid"));
    page->EditContext().EntitySelection().Clear();
    tree->Search(Format(u8"{}", lid).AsView());
    CHECK(page->EditContext().EntitySelection().Contains(lid));
    tree->Search(StringView{});
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Lid |   Barrel"));

    CHECK(tree->Tree()->GetModel()->SetData(tree->IndexOf(lid), gui::Variant(StringView(u8"Hatch"))));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Hatch |   Barrel"));
    page->Commands().Undo();
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Lid |   Barrel"));
    page->OnClose();
}

TEST_CASE("hierarchy: a row dropped onto another goes under it, between two moves there, onto the scene's row becomes a root; never inside itself")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneHierarchyView* tree = page->Hierarchy();
    REQUIRE(tree != nullptr);
    const Guid barrel = page->EditContext().CreateEntity(u8"Barrel");
    const Guid lid = page->EditContext().CreateEntity(u8"Lid", bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    const gui::IModel* model = tree->Tree()->GetModel();
    const auto drop = [&](const Guid& moved, const Guid& onto, gui::DropPlace place)
    {
        gui::ModelDrop d;
        d.Source = model;
        d.Ids.PushBack(model->IdOf(tree->IndexOf(moved)));
        d.Index = tree->IndexOf(onto);
        d.Place = place;
        return d;
    };

    CHECK(tree->Take(drop(barrel, bed.crate, gui::DropPlace::On)));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Lid |     Barrel"));
    CHECK(tree->Take(drop(barrel, lid, gui::DropPlace::Before)));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Barrel |     Lid"));
    CHECK(tree->Take(drop(lid, Guid{}, gui::DropPlace::On))); // onto the scene's row
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Barrel |   Lid"));
    CHECK_FALSE(tree->CanTake(drop(bed.crate, barrel, gui::DropPlace::On))); // inside itself
    CHECK_FALSE(tree->CanTake(drop(Guid{}, barrel, gui::DropPlace::On)));    // the scene stays

    // A drag carries its entities, for an entity slot.
    const gui::DragPayload payload = tree->DragOf(Span<const Guid>(&lid, 1u));
    Array<Guid> carried;
    CHECK(EntityDrag::Read(payload, carried));
    REQUIRE(carried.Size() == 1u);
    CHECK(carried[0] == lid);
    page->OnClose();
}

TEST_CASE("hierarchy: a prefab dropped in is spawned under the row; the menus and the add button are the scene's actions, off while it simulates")
{
    Bed bed;
    {
        // The prefab needs content: one root.
        scene::Scene authored(DefaultAllocator(), u8"barrel");
        (void)authored.CreateEntity(u8"Barrel");
        REQUIRE(scene::SavePrefab(authored, *bed.barrel).IsOk());
    }
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneHierarchyView* tree = page->Hierarchy();
    REQUIRE(tree != nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    const gui::DragPayload asset = app::AssetDrag::Payload(bed.barrel->Id(), u8"PrefabDocument", u8"Barrel");
    gui::ModelDrop drop;
    drop.Payload = &asset;
    drop.Index = tree->IndexOf(bed.crate);
    drop.Place = gui::DropPlace::On;
    CHECK(tree->Take(drop));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tree->Rows() == String(u8"Level |   Crate |     Barrel"));
    const gui::DragPayload level = app::AssetDrag::Payload(bed.level->Id(), u8"SceneDocument", u8"Level");
    drop.Payload = &level;
    CHECK_FALSE(tree->CanTake(drop)); // only a prefab goes in

    // A row's menu: its actions over the page.
    Shown shown(*page);
    tree->ShowMenu(tree->ItemOf(bed.crate), Float2{10.0f, 10.0f});
    REQUIRE(tree->OpenMenu() != nullptr);
    CHECK(page->EditContext().EntitySelection().Contains(bed.crate)); // the row it was asked over
    bool hasDelete = false;
    for (usize i = 0; i < tree->OpenMenu()->ChildCount(); ++i)
        if (auto* item = Cast<gui::MenuItem>(tree->OpenMenu()->GetChildAt(i)))
            hasDelete = hasDelete || item->GetText() == StringView(u8"Delete");
    CHECK(hasDelete);
    tree->OpenMenu()->Close();

    auto* add = FindId<gui::ToolButton>(*page->Content(), u8"hierarchy-add");
    REQUIRE(add != nullptr);
    CHECK(add->IsEnabled());
    CHECK(add->GetTooltipText().StartsWith(u8"Create Entity"));
    page->StartSimulation();
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(add->IsEnabled()); // refused visibly while it simulates
    page->StopSimulation();
    page->OnClose();
}

TEST_CASE("hierarchy: a scene opens with its top level shown; what is under an entity waits to be opened")
{
    Bed bed;
    {
        UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
        (void)page->EditContext().CreateEntity(u8"Lid", bed.crate);
        REQUIRE(page->Save().IsOk());
        page->OnClose();
    }
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(page->Hierarchy()->Rows() == String(u8"Level |   Crate"));
    page->OnClose();
}

TEST_CASE("inspector: nothing selected says so; an entity shows its head, Entity, Transform and each component; Add Component and its undo")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    REQUIRE(inspector != nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    RefPtr<gui::Node> content = page->Content();
    CHECK(FindId<gui::Node>(*content, u8"inspector-state")->IsDisplayed());
    CHECK_FALSE(inspector->Grid()->IsDisplayed());

    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(FindId<gui::Node>(*content, u8"inspector-state")->IsDisplayed());
    CHECK(FindId<gui::TextField>(*content, u8"inspector-name")->GetText() == StringView(u8"Crate"));
    CHECK(inspector->Kind() == StringView(u8"Entity"));
    CHECK(inspector->SectionTitles() == String(u8"Entity | Transform"));

    Array<String> addable = inspector->AddableComponents();
    bool light = false;
    for (const String& name : addable)
        light = light || name == String(u8"LightComponent");
    REQUIRE(light);
    CHECK(inspector->AddComponent(u8"LightComponent"));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->SectionTitles() == String(u8"Entity | Transform | Light"));
    page->Commands().Undo(); // one step
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->SectionTitles() == String(u8"Entity | Transform"));
    page->OnClose();
}

TEST_CASE("inspector: a transform edit and a component edit are each one undo step; two edits of one row are two")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    REQUIRE(inspector->AddComponent(u8"LightComponent"));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    scene::Scene& scene = *page->ScenePtr();
    const auto commit = [&](StringView path, f64 value)
    {
        gui::PropertyWrite write;
        write.Value = gui::Variant(value);
        inspector->Sections().BeginEdit(path);
        inspector->Sections().CommitEdit(path, write);
        page->OnUpdate(bed.host, 1.0f / 60.0f);
    };

    commit(inspector->PathOf(u8"Transform", u8"position.x").AsView(), 4.0);
    CHECK(scene.GetLocalTransform(scene.FindEntity(bed.crate)).position.x == doctest::Approx(4.0f));
    const String intensity = inspector->PathOf(u8"Light", u8"intensity");
    commit(intensity.AsView(), 7.0);
    commit(intensity.AsView(), 9.0);
    auto* manager = page->EditContext().FindManager(&TypeOf<engine::render::LightComponent>());
    REQUIRE(manager != nullptr);
    const auto lightOf = [&]() { return static_cast<engine::render::LightComponent*>(manager->GetComponentInstance(scene.FindEntity(bed.crate)).Pointer()); };
    CHECK(lightOf()->intensity == doctest::Approx(9.0f));
    page->Commands().Undo();
    CHECK(lightOf()->intensity == doctest::Approx(7.0f)); // the second edit alone
    page->Commands().Undo();
    CHECK(lightOf()->intensity != doctest::Approx(7.0f));
    page->Commands().Redo();
    CHECK(lightOf()->intensity == doctest::Approx(7.0f));
    page->Commands().Undo();
    page->Commands().Undo(); // the transform
    CHECK(scene.GetLocalTransform(scene.FindEntity(bed.crate)).position.x == doctest::Approx(0.0f));
    page->OnClose();
}

TEST_CASE("inspector: several entities edited at once; a component's header has its verbs; read only while the page simulates")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    const Guid lamp = page->EditContext().CreateEntity(u8"Lamp");
    const Guid both[] = {bed.crate, lamp};
    page->EditContext().EntitySelection().Set(Span<const Guid>(both, 2u));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->Kind() == StringView(u8"2 entities"));
    REQUIRE(inspector->AddComponent(u8"LightComponent")); // to both
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->SectionTitles() == String(u8"Entity | Transform | Light"));
    const String intensity = inspector->PathOf(u8"Light", u8"intensity");
    gui::PropertyWrite write;
    write.Value = gui::Variant(3.0);
    inspector->Sections().BeginEdit(intensity.AsView());
    inspector->Sections().CommitEdit(intensity.AsView(), write);
    scene::Scene& scene = *page->ScenePtr();
    auto* manager = page->EditContext().FindManager(&TypeOf<engine::render::LightComponent>());
    CHECK(static_cast<engine::render::LightComponent*>(manager->GetComponentInstance(scene.FindEntity(lamp)).Pointer())->intensity == doctest::Approx(3.0f));
    CHECK(static_cast<engine::render::LightComponent*>(manager->GetComponentInstance(scene.FindEntity(bed.crate)).Pointer())->intensity == doctest::Approx(3.0f));

    // The header's verbs: Remove Component takes it from both, one undo brings it back.
    Shown shown(*page);
    REQUIRE(inspector->OpenHeaderMenu(u8"#Light"));
    REQUIRE(inspector->Grid()->GetRowMenu() != nullptr);
    gui::MenuItem* remove = nullptr;
    for (usize i = 0; i < inspector->Grid()->GetRowMenu()->ChildCount(); ++i)
        if (auto* item = Cast<gui::MenuItem>(inspector->Grid()->GetRowMenu()->GetChildAt(i)); item != nullptr && item->GetText() == StringView(u8"Remove Component"))
            remove = item;
    REQUIRE(remove != nullptr);
    (void)remove->HandleActivate();
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->SectionTitles() == String(u8"Entity | Transform"));
    page->Commands().Undo();
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->SectionTitles() == String(u8"Entity | Transform | Light"));

    // Simulating: read only, and said.
    page->StartSimulation();
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->Sections().IsReadOnly());
    CHECK(FindId<gui::Node>(*page->Content(), u8"inspector-note")->IsDisplayed());
    write.Value = gui::Variant(5.0);
    inspector->Sections().BeginEdit(intensity.AsView());
    inspector->Sections().CommitEdit(intensity.AsView(), write);
    CHECK(static_cast<engine::render::LightComponent*>(manager->GetComponentInstance(scene.FindEntity(lamp)).Pointer())->intensity == doctest::Approx(3.0f));
    page->StopSimulation();
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(inspector->Sections().IsReadOnly());
    page->OnClose();
}

TEST_CASE("inspector: an entity reference is a slot over the scene's entities: its name, a pick from the entity picker (the hierarchy's tree), a dragged entity, one undo each")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    const Guid lamp = page->EditContext().CreateEntity(u8"Lamp");
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    REQUIRE(inspector->AddComponent(u8"InstanceMemberComponent"));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    Shown shown(*page);
    const String path = inspector->PathOf(u8"Instance Member", u8"instancedMesh");
    REQUIRE_FALSE(path.IsEmpty());
    auto* slot = Cast<EntityRefEditor>(inspector->Grid()->FindEditor(path.AsView()));
    REQUIRE(slot != nullptr); // the entity slot, not the asset one
    CHECK(slot->Slot()->ShownName() == StringView(u8"Not set"));

    // The pick: the entity picker, the scene as the hierarchy shows it; a search keeps a match.
    const i64 before = page->Commands().UndoIndex();
    REQUIRE(page->PickEntity(slot->Request()));
    EntityPickerWindow* picker = page->EntityPicker();
    REQUIRE(picker != nullptr);
    CHECK(picker->IsOpen());
    CHECK(picker->Rows() == page->Hierarchy()->Rows()); // the same tree
    CHECK(picker->Rows() == String(u8"Level |   Crate |   Lamp"));
    CHECK(picker->LitName().IsEmpty()); // nothing current
    CHECK(picker->Hint() == StringView(u8"Enter picks, Esc closes"));
    picker->Step(-1); // the scene's row: no pick
    picker->PickLit();
    CHECK(picker->IsOpen());
    picker->Search(u8"la");
    CHECK(picker->Rows() == String(u8"Level |   Lamp"));
    CHECK(picker->LitName() == String(u8"Lamp")); // the first match
    picker->Search(u8"nothing like it");
    CHECK(picker->State() == StringView(u8"No entity matches \"nothing like it\"."));
    picker->Search(u8"la");
    picker->PickLit();
    CHECK_FALSE(picker->IsOpen());
    CHECK(page->Commands().UndoIndex() == before + 1);
    CHECK(page->Commands().CanUndo());
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    (void)shown.root->UpdateLayout();
    scene::Scene& scene = *page->ScenePtr();
    auto* manager = page->EditContext().FindManager(&TypeOf<engine::render::InstanceMemberComponent>());
    REQUIRE(manager != nullptr);
    const auto member = [&]() { return static_cast<engine::render::InstanceMemberComponent*>(manager->GetComponentInstance(scene.FindEntity(bed.crate)).Pointer()); };
    CHECK(member()->instancedMesh.id == lamp);
    slot = Cast<EntityRefEditor>(inspector->Grid()->FindEditor(path.AsView()));
    REQUIRE(slot != nullptr);
    CHECK(slot->Slot()->ShownName() == StringView(u8"Lamp"));
    // Again: the current entity lit, the foot saying Esc keeps it; its close writes nothing (no
    // None to slip onto: the slot's own clear sets nothing).
    REQUIRE(page->PickEntity(slot->Request()));
    CHECK(picker->LitName() == String(u8"Lamp"));
    CHECK(picker->Hint() == StringView(u8"Enter picks, Esc keeps Lamp"));
    REQUIRE(picker->CloseButton() != nullptr);
    CHECK(picker->CloseButton()->HandleActivate());
    CHECK_FALSE(picker->IsOpen());
    CHECK(page->Commands().UndoIndex() == before + 1);
    page->Commands().Undo(); // one step
    CHECK(member()->instancedMesh.id.IsNil());

    // A dragged entity, as from the hierarchy.
    const gui::DragPayload dragged = EntityDrag::Payload(Span<const Guid>(&bed.crate, 1u), u8"Crate");
    CHECK(slot->Take(dragged));
    CHECK(member()->instancedMesh.id == bed.crate);
    page->OnClose();
}

TEST_CASE("scene page: the animation panel binds an entity picked from the scene; a clip opened while the page shows goes into it")
{
    Bed bed;
    pipeline::RegisterPropertyAnimationAssets();
    foundation::content::Instance* clip = pipeline::CreatePropertyAnimationClip(bed.project->SourceDb().RootGroup(), u8"Swing");
    REQUIRE(clip != nullptr);
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    PropertyAnimationPanel* panel = page->AnimationPanel();
    REQUIRE(panel != nullptr);
    const Guid door = page->EditContext().CreateEntity(u8"Door");
    page->OnUpdate(bed.host, 1.0f / 60.0f);

    // Off screen, a clip's open is not the page's: it opens as an asset would.
    CHECK_FALSE(page->IsOnScreen());
    CHECK_FALSE(bed.context.TryInterceptOpenAsset(*clip));

    Shown shown(*page);
    CHECK(page->IsOnScreen());
    page->EditContext().EntitySelection().Set(bed.crate);
    CHECK(bed.context.TryInterceptOpenAsset(*clip));
    CHECK(page->AnimationPanelShown());
    CHECK(panel->Document().ClipId() == clip->Id());
    CHECK(panel->BoundEntity() == bed.crate); // the selection, as Edit Clip binds it
    CHECK_FALSE(bed.context.TryInterceptOpenAsset(*bed.barrel)); // a prefab is not a clip

    // Pick: the entity picker, the bound entity lit; a pick binds, None keeps it.
    panel->PickEntity();
    EntityPickerWindow* picker = page->EntityPicker();
    REQUIRE(picker != nullptr);
    CHECK(picker->IsOpen());
    CHECK(picker->LitName() == String(u8"Crate"));
    picker->Search(u8"door");
    picker->PickLit();
    CHECK(panel->BoundEntity() == door);
    page->OnClose();
    CHECK_FALSE(bed.context.TryInterceptOpenAsset(*clip)); // closed: it let go
}

TEST_CASE("inspector: a bone name picks from the skeleton of the animator above, a name it lacks said so; with none, text")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    SceneEditContext& edit = page->EditContext();
    scene::Scene& scene = *page->ScenePtr();
    RefPtr<foundation::animation::Skeleton> skeleton = MakeRef<foundation::animation::Skeleton>(DefaultAllocator(), 3);
    {
        Array<foundation::animation::Bone>& bones = skeleton->Bones();
        const char8_t* names[] = {u8"Thigh", u8"Shin", u8"Foot"};
        for (i32 i = 0; i < 3; ++i)
        {
            bones[static_cast<usize>(i)].index = i;
            bones[static_cast<usize>(i)].name = String(names[i]);
            bones[static_cast<usize>(i)].parentIndex = i - 1;
        }
        skeleton->BuildNameMap();
    }
    const Guid rider = edit.CreateEntity(u8"Rider");
    const Guid leg = edit.CreateEntity(u8"LegIk", rider);
    auto* clips = scene.GetSystem<engine::animation::SkeletalAnimationComponentManager>();
    auto* legs = scene.GetSystem<engine::animation::TwoBoneIkComponentManager>();
    REQUIRE(clips != nullptr);
    REQUIRE(legs != nullptr);
    clips->Add(scene.FindEntity(rider)).skeleton.SetDirect(skeleton);
    legs->Add(scene.FindEntity(leg)).midBone = String(u8"Knee");
    const auto live = [&]() -> engine::animation::TwoBoneIkComponent& { return *legs->Get(scene.FindEntity(leg)); };

    edit.EntitySelection().Set(leg);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    Shown shown(*page);
    const String startPath = inspector->PathOf(u8"Two Bone IK", u8"startBone");
    REQUIRE_FALSE(startPath.IsEmpty());
    auto* start = Cast<BoneNameEditor>(inspector->Grid()->FindEditor(startPath.AsView()));
    REQUIRE(start != nullptr);
    CHECK(start->ListsBones());
    gui::ComboBox& choices = *start->GetComboBox();
    REQUIRE(choices.ItemCount() == 4u);
    CHECK(choices.GetItemsText() == String(u8"(none)|Thigh|Shin|Foot"));
    CHECK(choices.GetSelectedIndex() == 0);
    const i64 before = page->Commands().UndoIndex();
    choices.SelectByUser(1);
    CHECK(live().startBone == String(u8"Thigh"));
    CHECK(page->Commands().UndoIndex() == before + 1); // one step
    page->Commands().Undo();
    CHECK(live().startBone.IsEmpty());

    page->OnUpdate(bed.host, 1.0f / 60.0f);
    (void)shown.root->UpdateLayout();
    auto* mid = Cast<BoneNameEditor>(inspector->Grid()->FindEditor(inspector->PathOf(u8"Two Bone IK", u8"midBone").AsView()));
    REQUIRE(mid != nullptr);
    REQUIRE(mid->GetComboBox()->ItemCount() == 5u);
    CHECK(mid->GetComboBox()->GetItemsText().AsView().EndsWith(u8"|Knee (not in the skeleton)"));
    CHECK(mid->GetComboBox()->GetSelectedIndex() == 4);

    // With no animator above, the text field.
    const Guid stray = edit.CreateEntity(u8"Stray");
    legs->Add(scene.FindEntity(stray));
    edit.EntitySelection().Set(stray);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    (void)shown.root->UpdateLayout();
    auto* text = Cast<BoneNameEditor>(inspector->Grid()->FindEditor(inspector->PathOf(u8"Two Bone IK", u8"startBone").AsView()));
    REQUIRE(text != nullptr);
    CHECK_FALSE(text->ListsBones());
    page->OnClose();
}

TEST_CASE("inspector: Paste Component puts the copied component on the selection, one undo; over one it has, it asks first")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    SceneEditContext& edit = page->EditContext();
    const Guid lamp = edit.CreateEntity(u8"Lamp");
    edit.EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    REQUIRE(inspector->AddComponent(u8"InstanceMemberComponent"));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    Shown shown(*page);
    CHECK_FALSE(inspector->CanPasteComponent()); // nothing copied
    const TypeInfo& member = TypeOf<engine::render::InstanceMemberComponent>();
    Array<byte> blob = edit.CopyComponent(bed.crate, &member);
    REQUIRE_FALSE(blob.IsEmpty());
    bed.context.CopyToEditorClipboard(u8"component", Move(blob), u8"component");

    // Onto an entity without one: pasted at once, one step.
    edit.EntitySelection().Set(lamp);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->CanPasteComponent());
    scene::Scene& scene = *page->ScenePtr();
    auto* manager = edit.FindManager(&member);
    REQUIRE(manager != nullptr);
    CHECK_FALSE(manager->HasComponent(scene.FindEntity(lamp)));
    CHECK(inspector->PasteComponent());
    CHECK(inspector->PasteConfirm() == nullptr);
    CHECK(manager->HasComponent(scene.FindEntity(lamp)));
    page->Commands().Undo(); // one step
    CHECK_FALSE(manager->HasComponent(scene.FindEntity(lamp)));

    // Onto one that has it: asked; Cancel leaves it, Overwrite pastes.
    edit.EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    const i64 asked = page->Commands().UndoIndex();
    CHECK(inspector->PasteComponent());
    REQUIRE(inspector->PasteConfirm() != nullptr);
    CHECK(page->Commands().UndoIndex() == asked); // nothing yet
    inspector->PasteConfirm()->Close();
    CHECK(inspector->PasteComponentNow());
    CHECK(page->Commands().UndoIndex() > asked);
    page->Commands().Undo(); // one step
    CHECK(page->Commands().UndoIndex() == asked);
    page->OnClose();
}

TEST_CASE("inspector: a component's extras under its header: a rigid body's notice while it has no collider, a zone's Bake")
{
    Bed bed;
    {
        static const bool once = []()
        {
            BuiltinInspectorExtras::Register(InspectorExtrasRegistry::Get());
            return true;
        }();
        (void)once;
    }
    String said;
    bed.context.OnNotice = [&said](NoticeKind, StringView message) { said = String(message); };
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    REQUIRE(inspector->AddComponent(u8"RigidBodyComponent"));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    const String title = String(TypeAttrString(TypeOf<engine::physics::RigidBodyComponent>(), "displayName", u8"Rigid Body"));
    CHECK(inspector->ExtraText(title.AsView(), u8"Collision").IsEmpty()); // a box: it has its collider

    // Cooked with no shape: said, and gone again once it is a box (a value edit, not a rebuild).
    auto* bodies = page->ScenePtr()->GetSystem<engine::physics::RigidBodyComponentManager>();
    REQUIRE(bodies != nullptr);
    bodies->Get(page->ScenePtr()->FindEntity(bed.crate))->shape = foundation::physics::ShapeKind::Cooked;
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->ExtraText(title.AsView(), u8"Collision") == String(kBuiltinInspectorExtrasNoColliderText));
    bodies->Get(page->ScenePtr()->FindEntity(bed.crate))->shape = foundation::physics::ShapeKind::Box;
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->ExtraText(title.AsView(), u8"Collision").IsEmpty());

    // A zone's Bake: its button, and with no asset assigned it says what to do.
    REQUIRE(inspector->AddComponent(u8"NavMeshZoneComponent"));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    const String zone = String(TypeAttrString(TypeOf<engine::navigation::NavMeshZoneComponent>(), "displayName", u8"Nav Mesh Zone"));
    CHECK(inspector->ExtraText(zone.AsView(), u8"Navigation") == String(u8"Bake Navigation"));
    CHECK(inspector->RunExtra(zone.AsView(), u8"Navigation"));
    CHECK(said == String(u8"Assign a Navigation Zone asset to this zone before baking."));
    page->OnClose();
}

TEST_CASE("script behaviors: a list of behaviours, each its script, Enabled and interval; every edit one undo step; a revert its default")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneEditContext& edit = page->EditContext();
    edit.AddComponent(bed.crate, &TypeOf<engine::script::ScriptComponent>());
    ScriptBehaviorsSource source(edit, &bed.context, Span<const Guid>(&bed.crate, 1u));
    auto* manager = page->ScenePtr()->GetSystem<engine::script::ScriptComponentManager>();
    REQUIRE(manager != nullptr);
    const auto component = [&]() { return manager->Get(page->ScenePtr()->FindEntity(bed.crate)); };

    Array<gui::PropertyInfo> top;
    source.GetProperties(StringView{}, top);
    REQUIRE(top.Size() == 1u);
    CHECK(top[0].Kind == gui::PropertyKind::List);
    CHECK(source.GetValue(u8"behaviors", 0).AsInt() == 0);

    gui::ListEdit insert;
    insert.Kind = gui::ListEdit::Op::Insert;
    insert.Index = 0;
    CHECK(source.EditList(u8"behaviors", insert));
    REQUIRE(component()->behaviors.Size() == 1u);
    Array<gui::PropertyInfo> elements;
    source.GetProperties(u8"behaviors", elements);
    REQUIRE(elements.Size() == 1u);
    CHECK(elements[0].DisplayName == String(u8"Behavior 1"));
    Array<gui::PropertyInfo> rows;
    source.GetProperties(u8"behaviors.0", rows);
    REQUIRE(rows.Size() == 3u); // no script yet: its own rows only
    CHECK(rows[0].Kind == gui::PropertyKind::Reference);
    REQUIRE(rows[0].ReferenceType == &foundation::script::ScriptClass::StaticType());
    // Its registered type, by its name: what the asset picker asks the builders for (an
    // unregistered TypeOf named it "<value>", and the picker offered "<value>s").
    CHECK(StringView(reinterpret_cast<const utf8char*>(rows[0].ReferenceType->name)) == StringView(u8"ScriptClass"));

    gui::PropertyWrite off;
    off.Value = gui::Variant(false);
    source.CommitEdit(u8"behaviors.0.enabled", off);
    CHECK_FALSE(component()->behaviors[0].enabled);
    gui::PropertyWrite every;
    every.Value = gui::Variant(0.5);
    source.CommitEdit(u8"behaviors.0.interval", every);
    CHECK(component()->behaviors[0].updateInterval == doctest::Approx(0.5f));
    page->Commands().Undo(); // the interval alone
    CHECK(component()->behaviors[0].updateInterval == doctest::Approx(0.0f));
    CHECK_FALSE(component()->behaviors[0].enabled);
    CHECK(source.Revert(u8"behaviors.0.enabled"));
    CHECK(component()->behaviors[0].enabled);
    page->Commands().Undo(); // the revert
    page->Commands().Undo(); // the edit
    CHECK(component()->behaviors[0].enabled);
    page->Commands().Undo(); // the insert
    CHECK(component()->behaviors.IsEmpty());
    page->OnClose();
}

TEST_CASE("inspector: the scene selected shows its settings, a block a section; an edit lands in the scene's block, one undo step")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    Shown shown(*page);
    shown.Click(*page->Hierarchy()->Tree(), 0); // the scene's row
    CHECK(page->Hierarchy()->SceneSelected());
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->Kind() == StringView(u8"Scene"));
    CHECK(FindId<gui::TextField>(*page->Content(), u8"inspector-name")->GetText() == StringView(u8"Level"));
    CHECK_FALSE(FindId<gui::Node>(*page->Content(), u8"inspector-add")->IsDisplayed()); // no components to add
    SceneSettingsSource* environment = inspector->SettingsOf(u8"Environment");
    REQUIRE(environment != nullptr);

    auto* system = page->EditContext().FindSystemBySettingsType(&TypeOf<engine::render::EnvironmentSettings>());
    REQUIRE(system != nullptr);
    const auto turbidity = [&]() { return static_cast<engine::render::EnvironmentSettings*>(page->EditContext().SettingsEditTarget(&TypeOf<engine::render::EnvironmentSettings>(), Guid{}))->turbidity; };
    const f32 before = turbidity();
    gui::PropertyWrite write;
    write.Value = gui::Variant(7.5);
    environment->BeginEdit(u8"turbidity");
    environment->CommitEdit(u8"turbidity", write);
    CHECK(turbidity() == doctest::Approx(7.5f));
    page->Commands().Undo();
    CHECK(turbidity() == doctest::Approx(before));
    page->Commands().Redo();
    CHECK(turbidity() == doctest::Approx(7.5f));

    // A block that can use a profile offers to make one while it uses its own values.
    if (system->SettingsProfileType() != nullptr)
    {
        CHECK(inspector->ShowsProfileAction(u8"Environment", u8"Make Profile"));
        CHECK_FALSE(inspector->ShowsProfileAction(u8"Environment", u8"Open Profile"));
    }

    // An entity selected again: its inspector, not the scene's.
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(inspector->Kind() == StringView(u8"Entity"));
    page->OnClose();
}

TEST_CASE("inspector: the physics block's collision groups: Default first, a new group collides with all, a pair both ways, only the last removed")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    Shown shown(*page);
    shown.Click(*page->Hierarchy()->Tree(), 0); // the scene's row
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    gui::IPropertySource* groups = inspector->ExtraSource(u8"Physics", 0);
    REQUIRE(groups != nullptr);
    CHECK_FALSE(inspector->PathOf(u8"Physics", u8"groups").IsEmpty());
    using engine::physics::PhysicsSceneSettings;
    const auto settings = [&]()
    { return static_cast<PhysicsSceneSettings*>(page->EditContext().FindSystemBySettingsType(&TypeOf<PhysicsSceneSettings>())->SettingsInstance()); };

    // None stored: one, Default.
    CHECK(groups->GetValue(u8"groups", 0).AsInt() == 1);
    CHECK(groups->GetValue(u8"groups.0.name", 0).AsString() == StringView(u8"Default"));
    gui::ListEdit add;
    add.Kind = gui::ListEdit::Op::Insert;
    add.Index = 0; // at the end whatever the place
    CHECK(groups->EditList(u8"groups", add));
    REQUIRE(settings()->groupNames.Size() == 2u);
    CHECK(settings()->groupNames[0] == String(u8"Default"));
    CHECK(settings()->groupNames[1] == String(u8"Group 1"));
    CHECK(groups->GetValue(u8"groups.1.collides", 0).AsInt() == 3);
    Array<gui::PropertyInfo> rows;
    groups->GetProperties(u8"groups.1", rows);
    REQUIRE(rows.Size() == 1u); // its name; which pairs collide is the matrix's
    CHECK(rows[0].Name == String(u8"name"));
    rows.Clear();
    groups->GetProperties(StringView{}, rows);
    REQUIRE(rows.Size() == 2u);
    CHECK(rows[1].Hint == String(u8"CollisionMatrix"));
    CHECK(groups->GetValue(u8"matrix", 0).AsInt() == 2);

    // The grid: a row and a column a group, a box at each pair; a click is the pair's write.
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    const String matrixPath = inspector->PathOf(u8"Physics", u8"matrix");
    REQUIRE_FALSE(matrixPath.IsEmpty());
    REQUIRE(inspector->Grid()->FindRow(matrixPath.AsView()) >= 0);
    inspector->Grid()->ScrollToItem(inspector->Grid()->FindRow(matrixPath.AsView())); // under the fold
    (void)shown.root->UpdateLayout();
    auto* matrix = Cast<CollisionMatrixEditor>(inspector->Grid()->FindEditor(matrixPath.AsView()));
    REQUIRE(matrix != nullptr);
    CHECK(matrix->Count() == 2u);
    REQUIRE(matrix->Box(1, 0) != nullptr);
    CHECK(matrix->Box(0, 1) == nullptr); // above the diagonal: the same pair as (1, 0)
    CHECK(matrix->Box(1, 0)->IsChecked());
    CHECK(matrix->Box(1, 0)->HandleActivate()); // off: both ways
    CHECK((settings()->groupCollides[1] & 1u) == 0u);
    CHECK((settings()->groupCollides[0] & 2u) == 0u);
    page->Commands().Undo();
    CHECK((settings()->groupCollides[1] & 1u) != 0u);

    // Group 1 no longer collides with Default: both rows say so.
    gui::PropertyWrite off;
    off.Value = gui::Variant(i64{2});
    off.Mask = 1u;
    groups->CommitEdit(u8"groups.1.collides", off);
    CHECK((settings()->groupCollides[1] & 1u) == 0u);
    CHECK((settings()->groupCollides[0] & 2u) == 0u);
    CHECK(groups->GetValue(u8"groups.0.collides", 0).AsInt() == 1);
    gui::PropertyWrite rename;
    rename.Value = gui::Variant(StringView(u8"Players"));
    groups->CommitEdit(u8"groups.1.name", rename);
    CHECK(settings()->groupNames[1] == String(u8"Players"));
    page->Commands().Undo(); // the rename alone
    CHECK(settings()->groupNames[1] == String(u8"Group 1"));
    CHECK((settings()->groupCollides[0] & 2u) == 0u);

    // Only the last: the others keep their bodies' indices.
    gui::ListEdit remove;
    remove.Kind = gui::ListEdit::Op::Remove;
    remove.Index = 0;
    CHECK_FALSE(groups->EditList(u8"groups", remove));
    remove.Index = 1;
    CHECK(groups->EditList(u8"groups", remove));
    CHECK(groups->GetValue(u8"groups", 0).AsInt() == 1);
    remove.Index = 0;
    CHECK_FALSE(groups->EditList(u8"groups", remove)); // Default stays
    page->Commands().Undo(); // the remove
    CHECK(groups->GetValue(u8"groups", 0).AsInt() == 2);

    // The physics world's cap: the list says its most, and an add past it is refused.
    rows.Clear();
    groups->GetProperties(StringView{}, rows);
    CHECK(rows[0].MaxCount == foundation::physics::kCollisionGroupCount);
    while (static_cast<u32>(groups->GetValue(u8"groups", 0).AsInt()) < foundation::physics::kCollisionGroupCount)
        REQUIRE(groups->EditList(u8"groups", add));
    CHECK_FALSE(groups->EditList(u8"groups", add));
    CHECK(groups->GetValue(u8"groups", 0).AsInt() == static_cast<i64>(foundation::physics::kCollisionGroupCount));
    page->OnClose();
}

TEST_CASE("level script: the Level class's properties as rows; an edit an override on the scene's block, one undo step; a revert drops it")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneEditContext& edit = page->EditContext();
    using engine::script::SceneScriptSettings;
    const auto settings = [&]()
    { return static_cast<SceneScriptSettings*>(edit.FindSystemBySettingsType(&TypeOf<SceneScriptSettings>())->SettingsInstance()); };
    LevelScriptSource source(edit, &bed.context);
    Array<gui::PropertyInfo> none;
    source.GetProperties(StringView{}, none);
    CHECK(none.IsEmpty()); // no Level class: nothing to author

    RefPtr<foundation::script::ScriptClass> level = MakeRef<foundation::script::ScriptClass>(DefaultAllocator());
    level->className = String(u8"Level");
    foundation::script::ScriptPropertyDesc speed;
    speed.name = String(u8"speed");
    speed.hash = 41;
    speed.type = foundation::script::ScriptPropertyType::Float;
    speed.defaultValue.kind = speed.type;
    speed.defaultValue.number = 2.0;
    level->properties.PushBack(speed);
    foundation::script::ScriptPropertyDesc spawn;
    spawn.name = String(u8"spawn");
    spawn.hash = 42;
    spawn.type = foundation::script::ScriptPropertyType::Vec3;
    spawn.defaultValue.kind = spawn.type;
    level->properties.PushBack(spawn);
    settings()->script.SetDirect(level);
    source.Refresh();

    Array<gui::PropertyInfo> rows;
    source.GetProperties(StringView{}, rows);
    REQUIRE(rows.Size() == 2u);
    CHECK(rows[0].DisplayName == String(u8"speed"));
    CHECK(rows[0].Kind == gui::PropertyKind::Float);
    CHECK(rows[1].Kind == gui::PropertyKind::Group);
    Array<gui::PropertyInfo> parts;
    source.GetProperties(rows[1].Name.AsView(), parts);
    CHECK(parts.Size() == 3u);
    CHECK(source.GetValue(u8"p41", 0).AsFloat() == doctest::Approx(2.0));

    gui::PropertyWrite fast;
    fast.Value = gui::Variant(5.0);
    source.CommitEdit(u8"p41", fast);
    REQUIRE(settings()->FindOverride(41) != nullptr);
    CHECK(source.GetValue(u8"p41", 0).AsFloat() == doctest::Approx(5.0));
    gui::PropertyWrite y;
    y.Value = gui::Variant(3.0);
    source.CommitEdit(u8"p42.y", y);
    REQUIRE(settings()->FindOverride(42) != nullptr);
    CHECK(settings()->FindOverride(42)->value.vector.y == doctest::Approx(3.0f));
    page->Commands().Undo(); // the part alone
    CHECK(settings()->FindOverride(42) == nullptr);
    CHECK(settings()->FindOverride(41) != nullptr);
    CHECK(source.Revert(u8"p41"));
    CHECK(settings()->FindOverride(41) == nullptr);
    CHECK(source.GetValue(u8"p41", 0).AsFloat() == doctest::Approx(2.0));
    page->Commands().Undo(); // the revert
    CHECK(settings()->FindOverride(41) != nullptr);
    page->OnClose();
}

TEST_CASE("scene bar: the editing controls are the page's actions; Select, the space's icon, Snap and its increments")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    Shown shown(*page);
    SceneBar* bar = page->Bar();
    REQUIRE(bar != nullptr);
    CHECK(bar->Bound() == 16u); // save, undo, redo, discard, four transform, space, snap, vertex snap, Simulate's three, frame, the animation panel
    GizmoController* gizmos = page->Gizmos();
    REQUIRE(gizmos != nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);

    // The animation panel's toggle: shows it, checked while it shows.
    auto* animation = FindId<gui::ButtonBase>(*page->Content(), u8"scene-animation-panel");
    REQUIRE(animation != nullptr);
    CHECK_FALSE(animation->IsChecked());
    shown.Click(*animation);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(page->AnimationPanelShown());
    CHECK(animation->IsChecked());
    shown.Click(*animation);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(page->AnimationPanelShown());

    auto* select = FindId<gui::ButtonBase>(*page->Content(), u8"scene-select");
    auto* move = FindId<gui::ButtonBase>(*page->Content(), u8"scene-move");
    REQUIRE(select != nullptr);
    REQUIRE(move != nullptr);
    CHECK(move->IsChecked());
    CHECK_FALSE(select->IsChecked());
    CHECK(String(select->GetTooltipText()) == String(u8"Select (Q)"));
    shown.Click(*select);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(gizmos->HandlesShown());
    CHECK(select->IsChecked());
    CHECK_FALSE(move->IsChecked());
    shown.Click(*move);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(gizmos->HandlesShown());
    CHECK(gizmos->Mode() == GizmoMode::Translate);

    // The space's icon is the space it is in.
    auto* space = FindId<gui::ButtonBase>(*page->Content(), u8"scene-space");
    REQUIRE(space != nullptr);
    CHECK(space->GetIconSource() == String(u8"Editor/Icons/world.svg"));
    shown.Click(*space);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(gizmos->Space() == GizmoSpace::Local);
    CHECK(space->GetIconSource() == String(u8"Editor/Icons/local.svg"));

    // Snap, and its increments: a fixed step, then the grid's cell where the camera is.
    auto* snap = FindId<gui::ToolMenuButton>(*page->Content(), u8"scene-snap");
    REQUIRE(snap != nullptr);
    CHECK_FALSE(gizmos->SnapMode());
    CHECK(bed.context.Actions().Execute(SceneActionIds::kGizmoSnap, page.Get()).IsOk());
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(gizmos->SnapMode());
    CHECK(snap->IsChecked());
    bar->PickIncrement(SceneBar::SnapKind::Rotate, 2);
    CHECK(gizmos->Gizmo().rotateSnapDegrees == doctest::Approx(45.0f));
    bar->PickIncrement(SceneBar::SnapKind::Move, 1);
    CHECK(gizmos->Gizmo().translateSnap == doctest::Approx(0.25f));
    CHECK_FALSE(gizmos->MoveSnapsToGrid());
    bar->PickIncrement(SceneBar::SnapKind::Move, 5); // the grid's cell
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(gizmos->MoveSnapsToGrid());
    CHECK(gizmos->Gizmo().translateSnap > 0.0f);
    page->OnClose();
}

TEST_CASE("scene HUD: the chips say what is on and change it; the stats, the view gizmo, the zoom readout and the tool's status")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    Shown shown(*page);
    SceneHud* hud = page->Hud();
    REQUIRE(hud != nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    EditorCamera& camera = *page->ViewportCamera();

    // Projection: the chip names it, an axis view by its name, in the accent off perspective.
    CHECK(hud->ChipText(u8"chip-projection") == StringView(u8"Perspective"));
    CHECK_FALSE(hud->ChipOn(u8"chip-projection"));
    hud->SetProjection(SceneHud::Projection::Orthographic);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(camera.orthographic);
    CHECK(hud->ChipText(u8"chip-projection") == StringView(u8"Orthographic"));
    CHECK(hud->ChipOn(u8"chip-projection"));
    hud->LookAlong(Float3{0, 1, 0});
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->ChipText(u8"chip-projection") == StringView(u8"Top"));

    // Shading: Unlit is the albedo term; a texture names itself; Lit is the default, not marked.
    CHECK(hud->ChipText(u8"chip-shading") == StringView(u8"Lit"));
    hud->SetShading(foundation::render::ViewDebugSemantic::Albedo);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->ChipText(u8"chip-shading") == StringView(u8"Unlit"));
    CHECK(hud->ChipOn(u8"chip-shading"));
    hud->ShowTexture(u8"ssgi.filtered");
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->ChipText(u8"chip-shading") == StringView(u8"ssgi.filtered"));
    hud->SetShading(foundation::render::ViewDebugSemantic::Off);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->ChipText(u8"chip-shading") == StringView(u8"Lit"));
    CHECK_FALSE(hud->ChipOn(u8"chip-shading"));

    // The menus are built, their checks following the state.
    gui::Menu* overlays = hud->ChipMenu(u8"chip-overlays");
    REQUIRE(overlays != nullptr);
    CHECK_FALSE(hud->StatsShown());
    page->Overlays().showFps = true;
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->StatsShown());

    // The view gizmo's centre switches the projection; a knob looks along its axis.
    ViewGizmoView* gizmo = hud->Gizmo();
    REQUIRE(gizmo != nullptr);
    (void)shown.root->UpdateLayout();
    hud->SetProjection(SceneHud::Projection::Perspective);
    camera.yaw = 0.3f;
    camera.pitch = -0.3f;
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    (void)shown.root->UpdateLayout();
    CHECK(gizmo->PressAt(gizmo->Layout().center));
    CHECK(camera.orthographic);
    const ViewGizmoLayout& layout = gizmo->Layout();
    i32 plusX = -1;
    for (i32 i = 0; i < 6; ++i)
        if (layout.poles[i].positive && layout.poles[i].axis.x > 0.5f)
            plusX = i;
    REQUIRE(plusX >= 0);
    CHECK(gizmo->PressAt(layout.poles[plusX].screen));
    CHECK(Dot(camera.Forward(), Float3{-1, 0, 0}) > 0.999f); // looking down -X, from +X

    // The zoom readout shows after a wheel zoom, then fades.
    CHECK_FALSE(hud->ZoomShown());
    hud->Zoomed();
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->ZoomShown());
    for (i32 i = 0; i < 4; ++i)
        page->OnUpdate(bed.host, 1.0f);
    CHECK_FALSE(hud->ZoomShown());

    // The select tool's status in parts: its mode, space, snap and what is selected.
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->StatusText() == String(u8"Move | World | Crate"));
    page->Gizmos()->SetSnapMode(true);
    page->Gizmos()->Gizmo().translateSnap = 0.5f;
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->StatusText() == String(u8"Move | World | Snap 0.5 m | Crate"));
    page->Gizmos()->SetHandlesShown(false);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(hud->StatusText() == String(u8"Select | Crate"));
    page->OnClose();
}

namespace
{
    // Two brushes of one category, one with a panel; the second available only when told.
    struct FakeBrush final : IViewportTool
    {
        FakeBrush(StringView id, StringView name, bool* available) : m_id(id), m_name(name), m_available(available) {}
        [[nodiscard]] StringView Id() const override { return m_id.AsView(); }
        [[nodiscard]] StringView DisplayName() const override { return m_name.AsView(); }
        [[nodiscard]] StringView Category() const override { return u8"Brushes"; }
        [[nodiscard]] StringView Icon() const override { return u8"Editor/Icons/cube.svg"; }
        [[nodiscard]] bool IsAvailable() const override { return m_available == nullptr || *m_available; }
        [[nodiscard]] StringView UnavailableReason() const override { return u8"Paint needs something to paint."; }
        bool Update(const ViewportToolInput&) override { return false; }
        String m_id;
        String m_name;
        bool* m_available;
    };
    bool g_paintAvailable = false;
    struct FakeBrushes final : IViewportToolProvider
    {
        void CreateTools(ViewportToolManager& manager, const ViewportToolHostContext&) override
        {
            manager.Add(MakeUnique<FakeBrush>(DefaultAllocator(), u8"test.sculpt", u8"Sculpt", nullptr));
            manager.Add(MakeUnique<FakeBrush>(DefaultAllocator(), u8"test.paint", u8"Paint", &g_paintAvailable));
        }
    };
    struct FakeSculptPanel final : IViewportToolPanelProvider
    {
        [[nodiscard]] StringView ToolId() const override { return u8"test.sculpt"; }
        [[nodiscard]] RefPtr<gui::Node> CreatePanel(IViewportTool&, const ViewportToolHostContext&) override
        {
            auto body = MakeRef<gui::Panel>(DefaultAllocator());
            auto radius = MakeRef<gui::Label>(DefaultAllocator());
            radius->SetText(u8"Radius");
            body->AddChild(radius.Get());
            return body;
        }
    };
}

TEST_CASE("scene tools: each tool an action and on the bar by category; its panel under the chips, moved, kept, put back")
{
    static FakeBrushes brushes;
    static FakeSculptPanel panel;
    ViewportToolProviderRegistry::Get().Register(&brushes);
    ViewportToolPanelRegistry::Get().Register(&panel);
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    Shown shown(*page);
    SceneTools* tools = page->ToolsView();
    REQUIRE(tools != nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);

    // The actions: Paint disabled, saying why.
    EditorActionRegistry& actions = bed.context.Actions();
    const String sculpt = SceneTools::ActionOf(u8"test.sculpt");
    const String paint = SceneTools::ActionOf(u8"test.paint");
    REQUIRE(actions.Find(sculpt.AsView()) != nullptr);
    CHECK(actions.Find(sculpt.AsView())->menuPath == String(u8"Scene/Tools/Brushes/Sculpt"));
    CHECK(actions.IsEnabled(sculpt.AsView(), page.Get()));
    CHECK_FALSE(actions.IsEnabled(paint.AsView(), page.Get()));
    CHECK(actions.DisabledReason(paint.AsView(), page.Get()) == String(u8"Paint needs something to paint."));

    // On the bar: the category one menu, its items as the actions are.
    gui::ToolMenuButton* category = tools->CategoryButton(u8"Brushes");
    REQUIRE(category != nullptr);
    CHECK(category->GetIconSource() == String(u8"Editor/Icons/cube.svg"));
    CHECK_FALSE(category->IsChecked());

    // Sculpt on: the category lit, its panel under the chips in the tool's chrome.
    CHECK(actions.Execute(sculpt.AsView(), page.Get()).IsOk());
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(category->IsChecked());
    CHECK(actions.IsChecked(sculpt.AsView(), page.Get()));
    REQUIRE(tools->Panel() != nullptr);
    CHECK(tools->PanelTitle() == StringView(u8"Sculpt"));
    CHECK(tools->Place() == nullptr);

    // Under the chips until moved; dragged by its head anywhere in the view, kept inside it.
    shown.styles.ApplyTree(*shown.root); // the panel came after the sheet was applied
    (void)shown.root->UpdateLayout();
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    (void)shown.root->UpdateLayout();
    const gui::Rect chips = FindId<gui::Node>(*page->Content(), u8"hud-top-start")->GetScreenBounds();
    const gui::Rect viewport = FindId<gui::Node>(*page->Content(), u8"scene-viewport")->GetScreenBounds();
    const Float2 under{tools->Panel()->GetScreenBounds().x - viewport.x, tools->Panel()->GetScreenBounds().y - viewport.y};
    CHECK(tools->Panel()->GetScreenBounds().y == doctest::Approx(chips.y + chips.height)); // under the chips
    ToolPanelHead* head = tools->Head();
    REQUIRE(head != nullptr);
    const gui::Rect box = head->GetScreenBounds();
    const Float2 grab{box.x + 10.0f, box.y + box.height * 0.5f};
    gui::EventDispatcher& d = *shown.root->GetEventDispatcher();
    d.InjectMouseDown(grab, gui::MouseButton::Left);
    d.InjectMouseMove(grab + Float2{120.0f, 80.0f});
    d.InjectMouseUp(grab + Float2{120.0f, 80.0f}, gui::MouseButton::Left);
    REQUIRE(tools->Place() != nullptr);
    const Float2 place = *tools->Place();
    CHECK(place.x == doctest::Approx(under.x + 120.0f));
    CHECK(place.y == doctest::Approx(under.y + 80.0f));
    d.InjectMouseDown(grab + Float2{120.0f, 80.0f}, gui::MouseButton::Left);
    d.InjectMouseMove(grab + Float2{-5000.0f, 9000.0f}); // far outside: kept inside the view
    d.InjectMouseUp(grab + Float2{-5000.0f, 9000.0f}, gui::MouseButton::Left);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    (void)shown.root->UpdateLayout();
    REQUIRE(tools->Place() != nullptr);
    CHECK(tools->Place()->x == doctest::Approx(0.0f));
    const Float2 pane = FindId<gui::Node>(*page->Content(), u8"scene-viewport")->GetSize();
    CHECK(tools->Place()->y <= pane.y - tools->Panel()->GetSize().y + 0.5f);

    // Folded to its head, and back.
    tools->SetFolded(true);
    CHECK(tools->Folded());
    tools->SetFolded(false);

    // Put back under the chips: no place.
    tools->SetPlace(nullptr);
    CHECK(tools->Place() == nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    (void)shown.root->UpdateLayout();
    CHECK(tools->Panel()->GetScreenBounds().x - viewport.x == doctest::Approx(under.x));

    // Sculpt again: back to Select, no panel. Paint refused while unavailable, then taken.
    CHECK(actions.Execute(sculpt.AsView(), page.Get()).IsOk());
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(tools->Panel() == nullptr);
    CHECK_FALSE(category->IsChecked());
    g_paintAvailable = true;
    CHECK(actions.Execute(paint.AsView(), page.Get()).IsOk());
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(actions.IsChecked(paint.AsView(), page.Get()));
    CHECK(tools->Panel() == nullptr); // Paint has no panel
    g_paintAvailable = false;
    page->OnClose();
}

namespace
{
    // A mesh for any instance: what the inspector's mesh slot binds to.
    struct AnyMesh final : foundation::resource::IResourceFactory
    {
        [[nodiscard]] const TypeInfo* ProductType() const override { return &foundation::geometry::StaticMesh::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return ProductType(); }
        [[nodiscard]] RefPtr<Object> Create(foundation::resource::ResourceManager&, foundation::content::Instance&) override
        {
            return MakeRef<foundation::geometry::StaticMesh>(DefaultAllocator());
        }
    };
}

TEST_CASE("inspector: a reference set in the grid binds at once (a mesh shows without reopening); its undo lets it go")
{
    Bed bed;
    AnyMesh factory;
    foundation::resource::ResourceManager resources(DefaultAllocator(), bed.project->SourceDb());
    resources.AddFactory(&factory);
    bed.context.SetResources(&resources);
    foundation::content::Instance* rock = bed.project->SourceDb().RootGroup()->CreateInstance(u8"Rock", scene::SceneDocument::StaticType());
    REQUIRE(rock != nullptr);
    {
        UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
        SceneInspectorView* inspector = page->Inspector();
        page->EditContext().AddComponent(bed.crate, &TypeOf<engine::render::MeshComponent>());
        page->EditContext().EntitySelection().Set(bed.crate);
        page->OnUpdate(bed.host, 1.0f / 60.0f);
        auto* meshes = page->ScenePtr()->GetSystem<engine::render::MeshComponentManager>();
        REQUIRE(meshes != nullptr);
        const auto mesh = [&]() { return meshes->Get(page->ScenePtr()->FindEntity(bed.crate)); };
        CHECK(mesh()->mesh.Get() == nullptr);

        const String path = inspector->PathOf(u8"Mesh", u8"mesh");
        REQUIRE_FALSE(path.IsEmpty());
        utf8char chars[37];
        rock->Id().ToChars(chars);
        gui::PropertyWrite write;
        write.Value = gui::Variant(StringView(chars, 36));
        inspector->Sections().BeginEdit(path.AsView());
        inspector->Sections().CommitEdit(path.AsView(), write);
        CHECK(mesh()->mesh.id == rock->Id());
        CHECK(mesh()->mesh.Get() != nullptr); // bound now: it draws without a reopen
        page->Commands().Undo();
        CHECK(mesh()->mesh.id.IsNil());
        CHECK(mesh()->mesh.Get() == nullptr); // and let go on undo
        page->OnClose();
    }
    bed.context.SetResources(nullptr);
}

TEST_CASE("inspector: Add Component's picker lists each component by its display name under its category, the categories together")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    page->Inspector()->OpenAddComponent();
    REQUIRE_FALSE(bed.picked.Items.IsEmpty());
    const app::PickerItem* light = nullptr;
    for (const app::PickerItem& item : bed.picked.Items)
        if (item.Key == String(u8"LightComponent"))
            light = &item;
    REQUIRE(light != nullptr);
    CHECK(light->Name == String(u8"Light"));         // its displayName, not its type's name
    CHECK(light->Section == String(u8"Rendering"));  // its category
    CHECK_FALSE(light->Description.IsEmpty());        // its description, for the foot while lit
    CHECK(light->Detail.IsEmpty());                   // not on its row, where a sentence overran the name
    // Each category's rows together, so a section shows once.
    for (usize i = 2; i < bed.picked.Items.Size(); ++i)
        if (bed.picked.Items[i].Section != bed.picked.Items[i - 1].Section)
            for (usize j = 0; j + 1 < i; ++j)
                CHECK(bed.picked.Items[j].Section != bed.picked.Items[i].Section);
    page->OnClose();
}

namespace
{
    template <typename T>
    T* FirstOf(gui::Node& node)
    {
        if (auto* found = Cast<T>(&node))
            return found;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FirstOf<T>(*node.GetChildAt(i)))
                return found;
        return nullptr;
    }
}

TEST_CASE("inspector: a value typed into the grid, or scrubbed, stays (frames on, not put back)")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneInspectorView* inspector = page->Inspector();
    Shown shown(*page);
    page->EditContext().AddComponent(bed.crate, &TypeOf<engine::render::LightComponent>());
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    shown.styles.ApplyTree(*shown.root);
    (void)shown.root->UpdateLayout();
    auto* lights = page->ScenePtr()->GetSystem<engine::render::LightComponentManager>();
    REQUIRE(lights != nullptr);
    const auto intensity = [&]() { return lights->Get(page->ScenePtr()->FindEntity(bed.crate))->intensity; };

    gui::PropertyGrid* grid = inspector->Grid();
    REQUIRE(grid != nullptr);
    const String path = inspector->PathOf(u8"Light", u8"intensity");
    const i32 item = grid->FindRow(path.AsView());
    REQUIRE(item >= 0);
    grid->ScrollToItem(item);
    (void)shown.root->UpdateLayout();
    gui::ItemRow* row = grid->RowForItem(item);
    REQUIRE(row != nullptr);
    gui::NumberField* field = FirstOf<gui::NumberField>(*row);
    REQUIRE(field != nullptr);

    // Typed: click into it, select all, type, Return; then frames go by.
    gui::EventDispatcher& d = *shown.root->GetEventDispatcher();
    const gui::Rect box = field->GetScreenBounds();
    const Float2 at{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
    d.InjectMouseDown(at, gui::MouseButton::Left);
    d.InjectMouseUp(at, gui::MouseButton::Left);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::A), gui::KeyModCtrl);
    d.InjectText(u8"7.5");
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Return));
    for (i32 i = 0; i < 5; ++i)
    {
        page->OnUpdate(bed.host, 1.0f / 60.0f);
        (void)shown.root->UpdateLayout();
    }
    CHECK(intensity() == doctest::Approx(7.5f));
    page->OnClose();
}

TEST_CASE("camera preview: the selected camera's view titled with its name at its aspect; a pin keeps it; a stale pin clears")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    SceneCameraPreview* preview = page->CameraPreview();
    REQUIRE(preview != nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(preview->IsShown()); // no camera selected

    // A camera selected: shown, titled with it, at its aspect.
    const Guid eye = page->EditContext().CreateEntity(u8"Eye");
    page->EditContext().EntitySelection().Set(eye);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    REQUIRE(page->Inspector()->AddComponent(u8"CameraComponent"));
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(preview->IsShown());
    CHECK(preview->Target() == eye);
    CHECK(preview->Title() == StringView(u8"Eye"));
    CHECK(preview->PictureSize().x == doctest::Approx(kSceneCameraPreviewPictureWidth));
    CHECK(preview->PictureSize().y == doctest::Approx(109.0f)); // 16:9
    scene::Scene& scene = *page->ScenePtr();
    auto* cameras = scene.GetSystem<engine::render::CameraComponentManager>();
    REQUIRE(cameras != nullptr);
    cameras->Get(scene.FindEntity(eye))->aspect = 1.0f;
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(preview->PictureSize().y == doctest::Approx(kSceneCameraPreviewPictureWidth)); // square

    // Another entity selected: hidden, unless the camera is pinned.
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(preview->IsShown());
    page->EditContext().EntitySelection().Set(eye);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    preview->TogglePin();
    CHECK(preview->Pinned() == eye);
    page->EditContext().EntitySelection().Set(bed.crate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(preview->IsShown());
    CHECK(preview->Target() == eye);

    // The pinned camera deleted: the pin clears and the preview goes.
    page->EditContext().DestroyEntity(eye);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(preview->Pinned().IsNil());
    CHECK_FALSE(preview->IsShown());
    page->OnClose();
}

TEST_CASE("scene bar: Simulate, Pause and Stop are the page's actions, its state named beside them while it runs")
{
    Bed bed;
    UniquePtr<SceneEditorPage> page = bed.Open(*bed.level);
    Shown shown(*page);
    SceneBar* bar = page->Bar();
    REQUIRE(bar != nullptr);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    auto* simulate = FindId<gui::ButtonBase>(*page->Content(), u8"scene-simulate");
    auto* pause = FindId<gui::ButtonBase>(*page->Content(), u8"scene-pause");
    auto* stop = FindId<gui::ButtonBase>(*page->Content(), u8"scene-stop");
    REQUIRE(simulate != nullptr);
    REQUIRE(pause != nullptr);
    REQUIRE(stop != nullptr);
    CHECK(String(simulate->GetTooltipText()).AsView().StartsWith(u8"Simulate"));
    CHECK(simulate->IsEnabled());
    CHECK_FALSE(pause->IsEnabled());
    CHECK_FALSE(stop->IsEnabled());
    CHECK(bar->State().IsEmpty());
    // The buttons are centred beside the state: it keeps its place and one width whatever it says,
    // so they never move as the scene starts, pauses or stops.
    const auto pauseAt = [&]()
    {
        (void)shown.root->UpdateLayout();
        return pause->GetScreenBounds().x;
    };
    const f32 resting = pauseAt();

    shown.Click(*simulate);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(page->IsSimulating());
    CHECK(bar->State() == StringView(u8"Simulating"));
    CHECK_FALSE(simulate->IsEnabled());
    CHECK(stop->IsEnabled());
    CHECK(pauseAt() == doctest::Approx(resting));
    shown.Click(*pause);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(page->IsPaused());
    CHECK(bar->State() == StringView(u8"Paused"));
    CHECK(pause->IsChecked());
    CHECK(pauseAt() == doctest::Approx(resting));
    shown.Click(*stop);
    page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(page->IsSimulating());
    CHECK(bar->State().IsEmpty());
    CHECK(pauseAt() == doctest::Approx(resting));
    page->OnClose();
}

namespace
{
    // A keyboard holding the keys it is given.
    class HeldKeys final : public foundation::shell::IKeyboard
    {
    public:
        foundation::shell::KeyCode held = foundation::shell::KeyCode::Unknown;
        [[nodiscard]] bool IsKeyDown(foundation::shell::KeyCode key) const override { return key == held; }
        [[nodiscard]] bool IsKeyPressed(foundation::shell::KeyCode) const override { return false; }
        [[nodiscard]] bool IsKeyReleased(foundation::shell::KeyCode) const override { return false; }
        [[nodiscard]] foundation::shell::KeyModifiers Modifiers() const override { return foundation::shell::KeyModifiers::None; }
    };
}

TEST_CASE("scene viewport: Shift is read with the pointer, so a brush's Shift + wheel works while its panel has the keys")
{
    HeldKeys device;
    device.held = foundation::shell::KeyCode::LeftShift;
    HeldKeys focusedUp; // the view focused, Shift not held
    // The view focused: its own keyboard says.
    CHECK(editor::ShiftHeldOver(&device, &device, false));
    CHECK_FALSE(editor::ShiftHeldOver(&focusedUp, &device, true));
    // Not focused (the brush's panel has the keys): the device's Shift while the pointer is over it.
    CHECK(editor::ShiftHeldOver(nullptr, &device, true));
    CHECK_FALSE(editor::ShiftHeldOver(nullptr, &device, false)); // the pointer elsewhere
    device.held = foundation::shell::KeyCode::RightShift;
    CHECK(editor::ShiftHeldOver(nullptr, &device, true));
    CHECK_FALSE(editor::ShiftHeldOver(nullptr, nullptr, true));
}
