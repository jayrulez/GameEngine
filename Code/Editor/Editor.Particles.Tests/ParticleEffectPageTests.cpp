// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The particle effect page over its markup (Data/Editor/ParticleEffectPage.gml) and a scratch
// project, without a host (no preview): the default system the seed and Add System share; the
// systems and their modules a tree; the chosen system's rows from reflection, each shown where its
// render mode uses it, its modules added by type from what derives from the list's base, a curve
// one value; a module chosen opening in place; each edit, a system added and a module or a system
// removed one undo step; a system's sub-emitters; the preview started again only when the
// modules or the systems change.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.particles;
import particles.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.particles;

using namespace foundation::core;
using namespace editor;
namespace particles = foundation::particles;
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

    /// A scratch project holding one effect: "Sparks", the default system, renamed.
    struct Bed
    {
        String dir = String(u8"scratch_particle_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        UniquePtr<ParticleEffectEditorPage> page;
        foundation::content::Instance* instance = nullptr;

        Bed()
        {
            pipeline::RegisterParticleEffectAsset();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            instance = project->SourceDb().RootGroup()->CreateInstance(u8"Fountain", pipeline::ParticleEffectAsset::StaticType());
            REQUIRE(instance != nullptr);
            pipeline::ParticleEffectAsset asset;
            pipeline::SeedDefaultParticleEffect(asset.Effect());
            asset.Effect().GetSystem(0)->name = String(u8"Sparks");
            REQUIRE(instance->WriteObject(asset).IsOk());
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            page = MakeUnique<ParticleEffectEditorPage>(DefaultAllocator(), context, Move(services), *instance);
            REQUIRE_FALSE(page->LoadFailed());
        }
        ~Bed()
        {
            page.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }

        particles::ParticleEffect& Effect() { return *page->Effect(); }
        Optional<gui::PropertyInfo> Info(StringView parent, StringView name)
        {
            Array<gui::PropertyInfo> rows;
            page->SystemRows().GetProperties(parent, rows);
            for (gui::PropertyInfo& info : rows)
                if (info.Name == name)
                    return Optional<gui::PropertyInfo>(Move(info));
            return {};
        }
        void Edit(StringView path, const gui::Variant& value)
        {
            page->SystemRows().BeginEdit(path);
            page->SystemRows().CommitEdit(path, gui::PropertyWrite{value});
        }
    };
}

TEST_CASE("particle page: the default system the seed and Add System share")
{
    particles::ParticleEffect fx;
    pipeline::SeedDefaultParticleEffect(fx);
    REQUIRE(fx.SystemCount() == 1);
    particles::ParticleSystem& added = pipeline::AddDefaultParticleSystem(fx);
    REQUIRE(fx.SystemCount() == 2);
    CHECK(&added == fx.GetSystem(1));
    CHECK(added.emitter.mode == particles::EmissionMode::Continuous);
    CHECK(added.emitter.spawnRate == doctest::Approx(120.0f));
    CHECK(added.InitializerCount() == 4);
    CHECK(added.BehaviorCount() == 1);
}

TEST_CASE("particle page: the systems and their modules a tree, the chosen system's rows from reflection")
{
    Bed bed;
    CHECK(bed.page->ChosenSystem() == 0);
    CHECK(bed.page->Tree() == String(u8"Sparks\n  Emitter\n  Initializers\n    Lifetime\n    Velocity\n    Size\n    Color\n  Behaviors\n    Gravity\n  Sub-emitters"));
    CHECK(bed.page->Fact(u8"Systems") == String(u8"1"));
    CHECK(bed.page->Fact(u8"Particle Budget") == String(u8"2000"));

    // Its modes named; its texture shown for a billboard, its mesh not; as a mesh, the other way.
    const Optional<gui::PropertyInfo> render = bed.Info(StringView{}, u8"renderMode");
    REQUIRE(render.HasValue());
    CHECK(render->Kind == gui::PropertyKind::Enum);
    CHECK(render->Choices.Size() == 7u);
    CHECK(bed.Info(StringView{}, u8"textureRef")->Visible);
    CHECK(bed.Info(StringView{}, u8"textureRef")->Kind == gui::PropertyKind::Reference);
    CHECK_FALSE(bed.Info(StringView{}, u8"meshRef")->Visible);
    bed.Edit(u8"renderMode", gui::Variant(static_cast<i64>(4)));
    CHECK(bed.Effect().GetSystem(0)->renderMode == particles::ParticleRenderMode::Mesh);
    CHECK_FALSE(bed.Info(StringView{}, u8"textureRef")->Visible);
    CHECK(bed.Info(StringView{}, u8"meshRef")->Visible);
    CHECK(bed.Info(StringView{}, u8"maxParticles").HasValue());

    // The modules' lists add by type, from what derives from their bases: no list in the page.
    const Optional<gui::PropertyInfo> behaviors = bed.Info(StringView{}, u8"behaviors");
    REQUIRE(behaviors.HasValue());
    CHECK(behaviors->Kind == gui::PropertyKind::List);
    bool sizeOverLifetime = false;
    for (const String& type : behaviors->ElementTypes)
        sizeOverLifetime = sizeOverLifetime || type == String(u8"Size over Lifetime");
    CHECK(sizeOverLifetime);
    CHECK(behaviors->ElementTypes.Size() >= 13u);
}

TEST_CASE("particle page: a module added by type, its curve one value; each edit one undo step")
{
    Bed bed;
    const u32 restarts = bed.page->PreviewRestarts();
    gui::ListEdit add;
    add.Kind = gui::ListEdit::Op::Insert;
    add.Index = 1;
    add.ElementType = String(u8"Size over Lifetime");
    REQUIRE(bed.page->SystemRows().EditList(u8"behaviors", add));
    REQUIRE(bed.Effect().GetSystem(0)->BehaviorCount() == 2);
    CHECK(Cast<particles::SizeOverLifetimeBehavior>(bed.Effect().GetSystem(0)->GetBehavior(1)) != nullptr);
    CHECK(bed.page->Tree().AsView().ContainsIgnoreCase(StringView(u8"    Size over Lifetime")));
    CHECK(bed.page->PreviewRestarts() == restarts + 1); // its modules changed
    const Optional<gui::PropertyInfo> size = bed.Info(u8"behaviors.1", u8"size");
    REQUIRE(size.HasValue());
    CHECK(size->Kind == gui::PropertyKind::Curve);
    CHECK(size->MaxCount == static_cast<u32>(particles::kMaxCurveKeys));

    // A value edited: the preview runs on (the same modules); undone, the value comes back.
    bed.Edit(u8"emitter.spawnRate", gui::Variant(30.0));
    CHECK(bed.Effect().GetSystem(0)->emitter.spawnRate == doctest::Approx(30.0f));
    CHECK(bed.page->PreviewRestarts() == restarts + 1);
    bed.page->Commands().Undo();
    CHECK(bed.Effect().GetSystem(0)->emitter.spawnRate == doctest::Approx(120.0f));
    bed.page->Commands().Undo();
    CHECK(bed.Effect().GetSystem(0)->BehaviorCount() == 1);
    CHECK(bed.page->Tree().AsView().ContainsIgnoreCase(StringView(u8"Size over Lifetime")) == false);
}

TEST_CASE("particle page: a module chosen in the tree is removed as one step; a system added and removed, an effect keeping one")
{
    Bed bed;
    bed.page->Choose(ParticleTreeRow{ParticleRowKind::Initializer, 0, 1}); // Velocity
    REQUIRE(bed.page->RemoveChosen());
    CHECK(bed.Effect().GetSystem(0)->InitializerCount() == 3);
    CHECK(bed.page->Tree() == String(u8"Sparks\n  Emitter\n  Initializers\n    Lifetime\n    Size\n    Color\n  Behaviors\n    Gravity\n  Sub-emitters"));
    bed.page->Commands().Undo();
    CHECK(bed.Effect().GetSystem(0)->InitializerCount() == 4);

    // The one system stays.
    bed.page->Choose(ParticleTreeRow{ParticleRowKind::System, 0, -1});
    CHECK_FALSE(bed.page->RemoveChosen());
    CHECK(bed.Effect().SystemCount() == 1);

    bed.page->AddSystem();
    CHECK(bed.Effect().SystemCount() == 2);
    CHECK(bed.page->ChosenSystem() == 1);
    CHECK(bed.page->Fact(u8"Systems") == String(u8"2"));
    CHECK(bed.page->Tree().AsView().ContainsIgnoreCase(StringView(u8"\nSystem 2\n")));
    // The chosen system's name edited reaches the tree.
    bed.Edit(u8"name", gui::Variant(StringView(u8"Smoke")));
    CHECK(bed.page->Tree().AsView().ContainsIgnoreCase(StringView(u8"\nSmoke\n")));
    REQUIRE(bed.page->RemoveChosen());
    CHECK(bed.Effect().SystemCount() == 1);
    CHECK(bed.page->ChosenSystem() == 0);
    bed.page->Commands().Undo(); // the system back, its name with it
    CHECK(bed.Effect().SystemCount() == 2);
    CHECK(bed.Effect().GetSystem(1)->name == String(u8"Smoke"));
    bed.page->Commands().Undo();
    bed.page->Commands().Undo();
    CHECK(bed.Effect().SystemCount() == 1);
    CHECK(bed.page->ChosenSystem() == 0);
}

TEST_CASE("particle page: a system's sub-emitters, each into another system chosen by name, in its rows and its tree")
{
    Bed bed;
    bed.page->AddSystem();
    bed.Edit(u8"name", gui::Variant(StringView(u8"Smoke"))); // the system added, chosen
    bed.page->Choose(ParticleTreeRow{ParticleRowKind::System, 0, -1}); // Sparks spawns into Smoke
    gui::ListEdit add;
    add.Kind = gui::ListEdit::Op::Insert;
    add.Index = 0;
    REQUIRE(bed.page->SystemRows().EditList(u8"subEmitters", add));
    REQUIRE(bed.Effect().GetSystem(0)->subEmitters.Size() == 1u);
    CHECK(bed.Effect().GetSystem(1)->subEmitters.IsEmpty()); // the other system has none

    // Where it spawns: the other systems by name, never itself.
    const Optional<gui::PropertyInfo> into = bed.Info(u8"subEmitters.0", u8"childSystemIndex");
    REQUIRE(into.HasValue());
    CHECK(into->Kind == gui::PropertyKind::Enum);
    REQUIRE(into->Choices.Size() == 2u);
    CHECK(into->Choices[0].Name == String(u8"Nothing"));
    CHECK(into->Choices[1].Name == String(u8"Smoke"));
    bed.Edit(u8"subEmitters.0.childSystemIndex", gui::Variant(static_cast<i64>(1)));
    CHECK(bed.Effect().GetSystem(0)->subEmitters[0].childSystemIndex == 1);
    const Optional<gui::PropertyInfo> link = bed.Info(u8"subEmitters", u8"0");
    REQUIRE(link.HasValue());
    CHECK(link->DisplayName == String(u8"On death into Smoke"));
    // In the tree under the system that owns it, and chosen there, opened in place.
    CHECK(bed.page->Tree().AsView().ContainsIgnoreCase(StringView(u8"  Sub-emitters\n    On death into Smoke\nSmoke")));
    bed.page->Choose(ParticleTreeRow{ParticleRowKind::SubEmitter, 0, 0});
    CHECK(bed.page->ChosenSystem() == 0);

    // The system it spawns into removed: it does nothing, until the removal is undone.
    bed.page->Choose(ParticleTreeRow{ParticleRowKind::System, 1, -1});
    REQUIRE(bed.page->RemoveChosen());
    CHECK(bed.Effect().GetSystem(0)->subEmitters[0].childSystemIndex == -1);
    bed.page->Commands().Undo();
    CHECK(bed.Effect().SystemCount() == 2);
    CHECK(bed.Effect().GetSystem(0)->subEmitters[0].childSystemIndex == 1);

    // Taken away from the tree, as a module is.
    bed.page->Choose(ParticleTreeRow{ParticleRowKind::SubEmitter, 0, 0});
    REQUIRE(bed.page->RemoveChosen());
    CHECK(bed.Effect().GetSystem(0)->subEmitters.IsEmpty());
}

TEST_CASE("particle page: the asset written outside it (an agent's write) shows on a clean page; unsaved edits are kept")
{
    Bed bed;
    // Written from outside: a second system, named.
    pipeline::ParticleEffectAsset written;
    pipeline::SeedDefaultParticleEffect(written.Effect());
    written.Effect().GetSystem(0)->name = String(u8"Shells");
    pipeline::AddDefaultParticleSystem(written.Effect()).name = String(u8"Sparks");
    REQUIRE(bed.instance->WriteObject(written).IsOk());
    bed.page->OnAssetExternallyModified();
    CHECK(bed.Effect().SystemCount() == 2);
    CHECK(bed.page->Tree().AsView().StartsWith(StringView(u8"Shells\n")));
    CHECK(bed.page->Tree().AsView().ContainsIgnoreCase(StringView(u8"\nSparks\n")));
    CHECK(bed.page->Fact(u8"Systems") == String(u8"2"));
    // The rows are over the effect read, not the one it replaced.
    gui::ListEdit add;
    add.Kind = gui::ListEdit::Op::Insert;
    REQUIRE(bed.page->SystemRows().EditList(u8"subEmitters", add));
    CHECK(bed.Effect().GetSystem(0)->subEmitters.Size() == 1u);

    // With unsaved edits, another write leaves the page as it is.
    REQUIRE(bed.page->IsDirty());
    pipeline::ParticleEffectAsset again;
    pipeline::SeedDefaultParticleEffect(again.Effect());
    REQUIRE(bed.instance->WriteObject(again).IsOk());
    bed.page->OnAssetExternallyModified();
    CHECK(bed.Effect().SystemCount() == 2);
    CHECK(bed.Effect().GetSystem(0)->subEmitters.Size() == 1u);
}

TEST_CASE("particle page: a system folded in the tree stays folded when another row is chosen or the effect changes")
{
    Bed bed;
    auto* tree = app::FindById<gui::TreeView>(*bed.page->Content(), u8"particle-tree");
    REQUIRE(tree != nullptr);
    CHECK(tree->IsIdExpanded(0)); // a system opens the first time it shows
    tree->SetIdExpanded(0, false);
    bed.page->Choose(ParticleTreeRow{ParticleRowKind::Emitter, 0, -1});
    CHECK_FALSE(tree->IsIdExpanded(0));
    bed.Edit(u8"emitter.spawnRate", gui::Variant(30.0)); // an edit rebuilds the tree
    CHECK_FALSE(tree->IsIdExpanded(0));
    bed.page->AddSystem();
    CHECK_FALSE(tree->IsIdExpanded(0));
    // The new system's row opens.
    usize added = 0;
    for (usize node = 0; node < 64 && added == 0; ++node)
        if (node > 0 && tree->IsIdExpanded(static_cast<gui::ItemId>(node)))
            added = node;
    CHECK(added > 0);
}

TEST_CASE("particle page: without a preview the transport has nothing to play")
{
    Bed bed;
    CHECK_FALSE(bed.page->CanPlay());
    bed.page->Stop(); // nothing to stop: the effect untouched
    CHECK(bed.Effect().GetSystem(0)->emitter.isEmitting);
}

TEST_CASE("particle page: a tree row's menu adds a module by type, moves one, deletes it; a system renamed in place")
{
    Bed bed;
    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{1200.0f, 800.0f});
    root->AddChild(bed.page->Content().Get());
    (void)root->UpdateLayout();
    const auto item = [](gui::Node& menu, StringView text) -> gui::MenuItem*
    {
        Array<gui::Node*> pending;
        pending.PushBack(&menu);
        while (!pending.IsEmpty())
        {
            gui::Node* node = pending.Back();
            pending.PopBack();
            if (auto* found = Cast<gui::MenuItem>(node); found != nullptr && found->GetText() == text)
                return found;
            for (usize i = 0; i < node->ChildCount(); ++i)
                pending.PushBack(node->GetChildAt(i));
        }
        return nullptr;
    };
    gui::Node& owner = *bed.page->Content();

    // A module's: Move Down, one step, still chosen where it went.
    bed.page->ShowRowMenu(ParticleTreeRow{ParticleRowKind::Initializer, 0, 0}, owner, Float2{40.0f, 80.0f}); // Lifetime
    REQUIRE(bed.page->RowMenu() != nullptr);
    gui::MenuItem* down = item(*bed.page->RowMenu(), u8"Move Down");
    REQUIRE(down != nullptr);
    CHECK(down->HandleActivate());
    CHECK(bed.page->Tree() == String(u8"Sparks\n  Emitter\n  Initializers\n    Velocity\n    Lifetime\n    Size\n    Color\n  Behaviors\n    Gravity\n  Sub-emitters"));
    bed.page->Commands().Undo();
    CHECK(bed.page->Tree().AsView().ContainsIgnoreCase(StringView(u8"Initializers\n    Lifetime\n    Velocity")));
    CHECK_FALSE(bed.page->MoveModule(ParticleTreeRow{ParticleRowKind::Initializer, 0, 0}, -1)); // the first goes no higher

    // A folder's: its modules by type; one added at the end, chosen.
    bed.page->ShowRowMenu(ParticleTreeRow{ParticleRowKind::Behaviors, 0, -1}, owner, Float2{40.0f, 80.0f});
    REQUIRE(bed.page->RowMenu() != nullptr);
    gui::MenuItem* add = item(*bed.page->RowMenu(), u8"Add Size over Lifetime");
    REQUIRE(add != nullptr);
    CHECK(add->HandleActivate());
    REQUIRE(bed.Effect().GetSystem(0)->BehaviorCount() == 2);
    CHECK(Cast<particles::SizeOverLifetimeBehavior>(bed.Effect().GetSystem(0)->GetBehavior(1)) != nullptr);

    // Delete on a module's menu.
    bed.page->ShowRowMenu(ParticleTreeRow{ParticleRowKind::Behavior, 0, 1}, owner, Float2{40.0f, 80.0f});
    REQUIRE(bed.page->RowMenu() != nullptr);
    gui::MenuItem* remove = item(*bed.page->RowMenu(), u8"Delete");
    REQUIRE(remove != nullptr);
    CHECK(remove->HandleActivate());
    CHECK(bed.Effect().GetSystem(0)->BehaviorCount() == 1);

    // A system's: Delete off while it is the only one; renamed in place, one step.
    bed.page->ShowRowMenu(ParticleTreeRow{ParticleRowKind::System, 0, -1}, owner, Float2{40.0f, 80.0f});
    REQUIRE(bed.page->RowMenu() != nullptr);
    REQUIRE(item(*bed.page->RowMenu(), u8"Delete System") != nullptr);
    CHECK_FALSE(item(*bed.page->RowMenu(), u8"Delete System")->IsEnabled());
    bed.page->RowMenu()->Close();
    CHECK(bed.page->BeginRename(0));
    REQUIRE(bed.page->RenameSystem(0, u8"Embers"));
    CHECK(bed.Effect().GetSystem(0)->name == String(u8"Embers"));
    CHECK(bed.page->Tree().AsView().StartsWith(u8"Embers\n"));
    bed.page->Commands().Undo();
    CHECK(bed.Effect().GetSystem(0)->name == String(u8"Sparks"));
    root->RemoveChild(bed.page->Content().Get());
}
