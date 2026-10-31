// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The property animation panel over its markup (Data/Editor/PropertyAnimationPanel.xml), a scratch
// project and a scene, without a host: the state with no clip; a clip opened, an entity bound, and
// a track added holding the entity's current value; keying and removing the chosen tracks; the
// bar's Undo, Redo and Save acting on the clip alone; the keys it binds handled while it holds the
// focus (Undo walks the clip, never the scene), with the rest going on to the page; and the preview
// posing the entity and standing down while the scene simulates.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.propertyanimation;
import propertyanimation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.propertyanimation;

using namespace foundation::core;
using namespace editor;
namespace propanim = foundation::propertyanimation;
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

    // A scratch project with one clip, "Bounce", and a scene with one entity, "Box", at (1, 2, 3);
    // the panel in a root, under a node standing for the scene page.
    struct Bed
    {
        String dir = String(u8"scratch_anim_panel_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        scene::Scene sc{DefaultAllocator(), u8"Level"};
        scene::EntityHandle box;
        Guid clip;
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::Panel> page = MakeRef<gui::Panel>(DefaultAllocator());
        UniquePtr<PropertyAnimationPanel> panel;
        usize pageUndos = 0;
        usize pageOthers = 0;

        Bed()
        {
            pipeline::RegisterPropertyAnimationAssets();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Instance* instance = pipeline::CreatePropertyAnimationClip(project->SourceDb().RootGroup(), u8"Bounce");
            REQUIRE(instance != nullptr);
            clip = instance->Id();
            box = sc.CreateEntity(u8"Box");
            sc.SetLocalPosition(box, Float3{1.0f, 2.0f, 3.0f});

            root->SetSize(Float2{1200.0f, 400.0f});
            root->AddChild(page.Get());
            (void)page->OnCommand(u8"edit.undo", [this](gui::CommandEvent&) { ++pageUndos; });
            (void)page->OnCommand(u8"scene.frame", [this](gui::CommandEvent&) { ++pageOthers; });
            RefPtr<gui::Node> content = LoadMarkup(kPropertyAnimationPanelPath);
            REQUIRE(content.Get() != nullptr);
            page->AddChild(content.Get());
            panel = MakeUnique<PropertyAnimationPanel>(DefaultAllocator(), context, sc, content);
            const Guid boxId = sc.GetEntityId(box);
            panel->SelectedEntity = [boxId]() { return boxId; };
        }
        ~Bed()
        {
            panel.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        [[nodiscard]] Float3 Position() const { return sc.GetLocalTransform(box).position; }
        [[nodiscard]] const propanim::PropertyAnimationClip& Clip() { return panel->Document().Clip(); }
        AnimatablePropertyInfo PositionSeed()
        {
            for (const AnimatablePropertyInfo& seed : panel->OfferedTracks())
                if (seed.propertyPath == String(u8"position"))
                    return seed;
            FAIL("no position seed");
            return {};
        }
    };
}

TEST_CASE("animation panel: with no clip, a state says so; a clip opened, its name, unsaved once edited")
{
    Bed bed;
    auto* empty = app::FindById<gui::Node>(*bed.panel->Content(), u8"anim-empty");
    auto* body = app::FindById<gui::Node>(*bed.panel->Content(), u8"anim-body");
    auto* name = app::FindById<gui::Label>(*bed.panel->Content(), u8"anim-clip");
    auto* dirty = app::FindById<gui::Node>(*bed.panel->Content(), u8"anim-dirty");
    REQUIRE(empty != nullptr);
    REQUIRE(body != nullptr);
    CHECK(empty->IsDisplayed());
    CHECK_FALSE(body->IsDisplayed());
    CHECK(name->GetText() == StringView(u8"No clip"));

    REQUIRE(bed.panel->LoadClip(bed.clip));
    CHECK_FALSE(empty->IsDisplayed());
    CHECK(body->IsDisplayed());
    CHECK(name->GetText() == StringView(u8"Bounce"));
    CHECK_FALSE(dirty->IsDisplayed());
    bed.panel->SetLength(2.0f);
    CHECK(dirty->IsDisplayed());
    CHECK(bed.panel->Axis().Duration() == doctest::Approx(2.0f));
    REQUIRE(bed.panel->Save().IsOk());
    CHECK_FALSE(dirty->IsDisplayed());
}

TEST_CASE("animation panel: an entity bound, a track added holding what it shows, keyed and removed, each one step")
{
    Bed bed;
    REQUIRE(bed.panel->LoadClip(bed.clip));
    CHECK(bed.panel->OfferedTracks().IsEmpty()); // no entity yet
    // Use Selected is enabled from the start (the page's selection is read when it is clicked).
    auto* bind = app::FindById<gui::ButtonBase>(*bed.panel->Content(), u8"anim-bind");
    REQUIRE(bind != nullptr);
    CHECK(bind->IsEnabled());
    bed.panel->BindSelected();
    CHECK(bed.panel->BoundEntity() == bed.sc.GetEntityId(bed.box));
    auto* entity = app::FindById<gui::Label>(*bed.panel->Content(), u8"anim-entity");
    CHECK(entity->GetText() == StringView(u8"Box"));

    // Add Track offers the transform's three first; position is added and keyed with the box's
    // position.
    REQUIRE(bed.panel->OfferedTracks().Size() >= 3u);
    bed.panel->AddTrack(bed.PositionSeed());
    REQUIRE(bed.Clip().tracks.Size() == 1u);
    CHECK(bed.Clip().tracks[0].channels[1].Evaluate(0.0f) == doctest::Approx(2.0f));
    for (const AnimatablePropertyInfo& seed : bed.panel->OfferedTracks())
        CHECK(seed.propertyPath != String(u8"position")); // offered once

    // With the box moved and the playhead at 1 s, Key keys every track (none chosen) with the
    // box's current position.
    bed.panel->StopPreview();
    bed.sc.SetLocalPosition(bed.box, Float3{5.0f, 6.0f, 7.0f});
    bed.panel->Scrub(1.0f);
    // The scrub posed the box from the clip (at 1 s it holds the first key); move it again, as a
    // gizmo would, and key that.
    bed.sc.SetLocalPosition(bed.box, Float3{5.0f, 6.0f, 7.0f});
    CHECK(bed.panel->KeyChosen() == 1u);
    CHECK(bed.Clip().tracks[0].channels[0].KeyCount() == 2u);
    CHECK(bed.Clip().tracks[0].channels[0].Evaluate(1.0f) == doctest::Approx(5.0f));

    // Choosing a key in the dopesheet chooses its track; Remove takes the track away, and one undo
    // brings it back.
    const gui::TimelineKeyRef first{0, 0};
    bed.panel->Sheet()->SetSelection(Span<const gui::TimelineKeyRef>(&first, 1u));
    REQUIRE(bed.panel->ChosenTracks().Size() == 1u);
    REQUIRE(bed.panel->RemoveChosen());
    CHECK(bed.Clip().tracks.IsEmpty());
    bed.panel->Undo();
    CHECK(bed.Clip().tracks.Size() == 1u);
    bed.panel->Redo();
    CHECK(bed.Clip().tracks.IsEmpty());
}

TEST_CASE("animation panel: the curve view's values fit what it shows when that changes, not as a key is edited")
{
    Bed bed;
    REQUIRE(bed.panel->LoadClip(bed.clip));
    bed.panel->BindSelected();
    bed.panel->AddTrack(bed.PositionSeed()); // keys at 1, 2 and 3 (where the box is)
    bed.panel->Update(0.0f, false);
    auto* curves = app::FindById<gui::CurveView>(*bed.panel->Content(), u8"anim-curves");
    REQUIRE(curves != nullptr);
    CHECK(curves->ValueLow() < 1.0f);
    CHECK(curves->ValueHigh() > 3.0f);
    CHECK(curves->ValueHigh() < 4.0f); // fitted, with a tenth of margin either side
    // Editing a key's value does not refit (the view would jump under the drag).
    const f32 high = curves->ValueHigh();
    bed.panel->Document().Edit(u8"test", [](propanim::PropertyAnimationClip& c) { c.tracks[0].channels[2].Keys()[0].value = 50.0f; });
    bed.panel->Update(0.0f, false);
    CHECK(curves->ValueHigh() == doctest::Approx(high));
    // Frame shows everything, the new value included.
    bed.panel->FrameCurves();
    CHECK(curves->ValueHigh() >= 50.0f);
}

TEST_CASE("animation panel: its keys are the clip's while it holds the focus; the rest go on to the page")
{
    Bed bed;
    REQUIRE(bed.panel->LoadClip(bed.clip));
    bed.panel->SetLength(3.0f);
    REQUIRE(bed.panel->Document().Commands().CanUndo());
    REQUIRE(bed.panel->Sheet() != nullptr);
    bed.panel->Sheet()->RequestFocus();
    REQUIRE(bed.panel->Sheet()->IsFocused());

    // Ctrl+Z's command undoes the clip's step, and the page is never told.
    CHECK(bed.root->RunCommand(u8"edit.undo"));
    CHECK_FALSE(bed.panel->Document().Commands().CanUndo());
    CHECK(bed.pageUndos == 0u);
    // With nothing left to undo, the command still belongs to the clip and never reaches the scene.
    (void)bed.root->RunCommand(u8"edit.undo");
    CHECK(bed.pageUndos == 0u);
    // A key it does not bind goes on to the page.
    CHECK(bed.root->RunCommand(u8"scene.frame"));
    CHECK(bed.pageOthers == 1u);

    // With the focus elsewhere, Undo is the page's again.
    bed.panel->Sheet()->ReleaseFocus();
    RefPtr<gui::Button> other = MakeRef<gui::Button>(DefaultAllocator());
    bed.page->AddChild(other.Get());
    other->RequestFocus();
    (void)bed.root->RunCommand(u8"edit.undo");
    CHECK(bed.pageUndos == 1u);
}

TEST_CASE("animation panel: a scrub poses the bound entity, put back when it stops; nothing posed while simulating")
{
    Bed bed;
    REQUIRE(bed.panel->LoadClip(bed.clip));
    bed.panel->BindSelected();
    bed.panel->AddTrack(bed.PositionSeed());            // a key at 0 s: (1, 2, 3)
    bed.panel->StopPreview();
    bed.sc.SetLocalPosition(bed.box, Float3{9.0f, 9.0f, 9.0f});
    bed.panel->Scrub(1.0f);
    bed.sc.SetLocalPosition(bed.box, Float3{9.0f, 9.0f, 9.0f});
    (void)bed.panel->KeyChosen();                        // a key at 1 s: (9, 9, 9)
    bed.panel->StopPreview();
    bed.sc.SetLocalPosition(bed.box, Float3{0.0f, 0.0f, 0.0f}); // where the scene has it

    bed.panel->Scrub(0.0f);
    CHECK(bed.Position().x == doctest::Approx(1.0f)); // posed
    CHECK(bed.panel->Preview().IsPosing());
    // Playing on: the transport moves the playhead and the pose.
    bed.panel->Play();
    bed.panel->Update(0.5f, false);
    CHECK(bed.panel->Axis().Playhead() == doctest::Approx(0.5f));
    CHECK(bed.Position().x > 1.0f);
    bed.panel->Pause();

    // While simulating, the preview stands down and the scene's own value is back.
    bed.panel->Update(0.0f, true);
    CHECK_FALSE(bed.panel->Preview().IsPosing());
    CHECK(bed.Position().x == doctest::Approx(0.0f));
    bed.panel->Scrub(0.25f);
    CHECK(bed.Position().x == doctest::Approx(0.0f)); // no pose while simulating
    bed.panel->Update(0.0f, false);
    bed.panel->Scrub(0.0f);
    CHECK(bed.Position().x == doctest::Approx(1.0f));
    bed.panel->StopPreview();
    CHECK(bed.Position().x == doctest::Approx(0.0f));
}
