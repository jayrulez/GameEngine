// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The particle effect page over its markup (Data/Editor/ParticleEffectPage.gml) and a scratch
// project, without a host (no preview): the default system the seed and Add System share; the
// systems and their modules a tree; the chosen system's rows from reflection, each shown where its
// render mode uses it, its modules added by type from what derives from the list's base, a curve
// one value; a module chosen opening in place; each edit, a system added and a module or a system
// removed one undo step; the effect's sub-emitter links; the preview started again only when the
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
    CHECK(bed.page->Tree() == String(u8"Sparks\n  Emitter\n  Initializers\n    Lifetime\n    Velocity\n    Size\n    Color\n  Behaviors\n    Gravity\nSub-emitters"));
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
    CHECK(bed.page->Tree() == String(u8"Sparks\n  Emitter\n  Initializers\n    Lifetime\n    Size\n    Color\n  Behaviors\n    Gravity\nSub-emitters"));
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

TEST_CASE("particle page: the effect's sub-emitter links, each into a system chosen by name, named by what sets it off")
{
    Bed bed;
    bed.page->AddSystem();
    bed.Edit(u8"name", gui::Variant(StringView(u8"Smoke"))); // the system added, chosen
    gui::ListEdit add;
    add.Kind = gui::ListEdit::Op::Insert;
    add.Index = 0;
    REQUIRE(bed.page->EffectRows().EditList(u8"subEmitterLinks", add));
    REQUIRE(bed.Effect().SubEmitterLinks().Size() == 1u);
    CHECK(bed.page->Fact(u8"Sub-emitters") == String(u8"1"));

    // Its system a choice by name; chosen, the link says it.
    Array<gui::PropertyInfo> rows;
    bed.page->EffectRows().GetProperties(u8"subEmitterLinks.0", rows);
    const gui::PropertyInfo* into = nullptr;
    for (const gui::PropertyInfo& info : rows)
        if (info.Name == String(u8"childSystemIndex"))
            into = &info;
    REQUIRE(into != nullptr);
    CHECK(into->Kind == gui::PropertyKind::Enum);
    REQUIRE(into->Choices.Size() == 3u);
    CHECK(into->Choices[0].Name == String(u8"Nothing"));
    CHECK(into->Choices[2].Name == String(u8"Smoke"));
    bed.page->EffectRows().BeginEdit(u8"subEmitterLinks.0.childSystemIndex");
    bed.page->EffectRows().CommitEdit(u8"subEmitterLinks.0.childSystemIndex", gui::PropertyWrite{gui::Variant(static_cast<i64>(1))});
    CHECK(bed.Effect().SubEmitterLinks()[0].childSystemIndex == 1);
    Array<gui::PropertyInfo> links;
    bed.page->EffectRows().GetProperties(u8"subEmitterLinks", links);
    REQUIRE(links.Size() == 1u);
    CHECK(links[0].DisplayName == String(u8"On death into Smoke"));

    // The effect's name and systems are not its rows: the tree has the systems.
    Array<gui::PropertyInfo> top;
    bed.page->EffectRows().GetProperties(StringView{}, top);
    for (const gui::PropertyInfo& info : top)
        CHECK((info.Name == String(u8"subEmitterLinks") || !info.Visible));

    // The system it spawns into removed: the link does nothing, until the removal is undone.
    bed.page->Choose(ParticleTreeRow{ParticleRowKind::System, 1, -1});
    REQUIRE(bed.page->RemoveChosen());
    CHECK(bed.Effect().SubEmitterLinks()[0].childSystemIndex == -1);
    bed.page->Commands().Undo();
    CHECK(bed.Effect().SystemCount() == 2);
    CHECK(bed.Effect().SubEmitterLinks()[0].childSystemIndex == 1);

    bed.page->Choose(ParticleTreeRow{ParticleRowKind::SubEmitters, -1, -1});
    CHECK(bed.page->ChosenSystem() == 0); // the system shown stays
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
    // The effect's rows are over the effect read, not the one it replaced.
    gui::ListEdit add;
    add.Kind = gui::ListEdit::Op::Insert;
    REQUIRE(bed.page->EffectRows().EditList(u8"subEmitterLinks", add));
    CHECK(bed.Effect().SubEmitterLinks().Size() == 1u);

    // With unsaved edits, another write leaves the page as it is.
    REQUIRE(bed.page->IsDirty());
    pipeline::ParticleEffectAsset again;
    pipeline::SeedDefaultParticleEffect(again.Effect());
    REQUIRE(bed.instance->WriteObject(again).IsOk());
    bed.page->OnAssetExternallyModified();
    CHECK(bed.Effect().SystemCount() == 2);
    CHECK(bed.Effect().SubEmitterLinks().Size() == 1u);
}

TEST_CASE("particle page: without a preview the transport has nothing to play")
{
    Bed bed;
    CHECK_FALSE(bed.page->CanPlay());
    bed.page->Stop(); // nothing to stop: the effect untouched
    CHECK(bed.Effect().GetSystem(0)->emitter.isEmitting);
}
