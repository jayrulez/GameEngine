// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The bus layout's rules (the cycle guard, the tree, adding, removing, renaming and moving a bus) as
// pure functions over the asset, the asset round-tripping through the binary serializer, and the
// page over its markup without a host: its tree, the chosen bus's rows, an edit one undo step.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.audio;
import audio.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.audio;

using namespace foundation::core;

TEST_CASE("bus layout page: AudioBusWouldCycle catches direct + transitive cycles")
{
    pipeline::AudioBusLayoutAsset asset;
    // A -> B -> C (parents point UP the chain).
    asset.custom[0].name = String(u8"A");
    asset.custom[0].parent = String(u8"B");
    asset.custom[1].name = String(u8"B");
    asset.custom[1].parent = String(u8"C");
    asset.custom[2].name = String(u8"C");
    asset.custom[2].parent = String(u8"Master");

    // Re-parenting C under A closes the loop (transitively). Under B likewise. Under a
    // fixed bus never cycles.
    CHECK(editor::AudioBusWouldCycle(asset, 2, u8"A"));
    CHECK(editor::AudioBusWouldCycle(asset, 2, u8"B"));
    CHECK_FALSE(editor::AudioBusWouldCycle(asset, 2, u8"Effects"));
    CHECK_FALSE(editor::AudioBusWouldCycle(asset, 2, u8"Master"));
    // A under C is the DIRECT ancestor case.
    CHECK(editor::AudioBusWouldCycle(asset, 0, u8"A"));
    // A under an unused name: no slot resolves - no cycle.
    CHECK_FALSE(editor::AudioBusWouldCycle(asset, 0, u8"Nope"));
}

TEST_CASE("bus layout page: asset round-trips buses + custom slots (undo blob path)")
{
    pipeline::AudioBusLayoutAsset a;
    a.music.volume = 0.3f;
    a.effects.lowpassHz = 1500.0f;
    a.custom[0].name = String(u8"drums");
    a.custom[0].parent = String(u8"Effects");
    a.custom[0].bus.lowpassHz = 1200.0f;
    a.custom[0].bus.reverbWet = 0.4f;

    // The custom-slot bank is gated on ar.Version() >= 2: the blob must ride a versioned
    // payload exactly like the page's undo snapshots (a raw serializer would DROP the bank).
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, pipeline::AudioBusLayoutAsset::StaticType());
        a.Serialize(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    pipeline::AudioBusLayoutAsset b;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, pipeline::AudioBusLayoutAsset::StaticType());
        b.Serialize(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(b.music.volume == doctest::Approx(0.3f));
    CHECK(b.effects.lowpassHz == doctest::Approx(1500.0f));
    CHECK(b.custom[0].name.AsView() == u8"drums");
    CHECK(b.custom[0].parent.AsView() == u8"Effects");
    CHECK(b.custom[0].bus.lowpassHz == doctest::Approx(1200.0f));
    CHECK(b.custom[0].bus.reverbWet == doctest::Approx(0.4f));
}

TEST_CASE("sound cue page: asset round-trips slots + jitter (undo blob path)")
{
    pipeline::SoundCueAsset a;
    a.clipIds[0] = Guid{0x11, 0x22};
    a.clipIds[2] = Guid{0x33, 0x44};
    a.weights[0] = 2.5f;
    a.weights[2] = 0.5f;
    a.mode = 2;
    a.pitchMin = 0.9f;
    a.pitchMax = 1.1f;
    a.volumeMin = 0.8f;
    a.volumeMax = 1.0f;

    // The page's undo snapshots ride a versioned payload; the round-trip must preserve
    // every field an undo restores.
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, pipeline::SoundCueAsset::StaticType());
        a.Serialize(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    pipeline::SoundCueAsset b;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, pipeline::SoundCueAsset::StaticType());
        b.Serialize(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(b.clipIds[0] == a.clipIds[0]);
    CHECK(b.clipIds[1].IsNil());
    CHECK(b.clipIds[2] == a.clipIds[2]);
    CHECK(b.weights[0] == doctest::Approx(2.5f));
    CHECK(b.weights[2] == doctest::Approx(0.5f));
    CHECK(b.mode == 2);
    CHECK(b.pitchMin == doctest::Approx(0.9f));
    CHECK(b.volumeMax == doctest::Approx(1.0f));
}

namespace
{
    // Effects < Footsteps < Steps (a custom under a custom), and Music < Ambience.
    void FillLayout(pipeline::AudioBusLayoutAsset& asset)
    {
        asset.custom[0].name = String(u8"Footsteps");
        asset.custom[0].parent = String(u8"Effects");
        asset.custom[1].name = String(u8"Ambience");
        asset.custom[1].parent = String(u8"music"); // a fixed bus's name in any case
        asset.custom[2].name = String(u8"Steps");
        asset.custom[2].parent = String(u8"Footsteps");
    }

    String TreeOf(const pipeline::AudioBusLayoutAsset& asset)
    {
        String out;
        for (const editor::BusRow& row : editor::BusTree(asset))
        {
            if (!out.IsEmpty())
                out += u8" | ";
            for (i32 d = 0; d < row.depth; ++d)
                out += u8".";
            out += row.name;
        }
        return out;
    }
}

TEST_CASE("bus layout model: the tree, each custom bus under its parent")
{
    pipeline::AudioBusLayoutAsset asset;
    FillLayout(asset);
    CHECK(TreeOf(asset) == String(u8"Master | .Effects | ..Footsteps | ...Steps | .Music | ..Ambience | .UI"));
    // A parent no bus has is Master, as the cook takes it.
    asset.custom[1].parent = String(u8"Nowhere");
    CHECK(TreeOf(asset) == String(u8"Master | .Effects | ..Footsteps | ...Steps | .Music | .UI | .Ambience"));
    // Buses under each other in a loop are reached from no fixed bus: shown under Master all the same.
    asset.custom[0].parent = String(u8"Steps");
    CHECK(editor::HasBusCycle(asset));
    CHECK(TreeOf(asset) == String(u8"Master | .Effects | .Music | .UI | .Ambience | .Footsteps | .Steps"));
}

TEST_CASE("bus layout model: added in the first free slot, removed with its children to Master, renamed with its children following")
{
    pipeline::AudioBusLayoutAsset asset;
    FillLayout(asset);
    const i32 added = editor::AddBus(asset);
    CHECK(added == 3);
    CHECK(asset.custom[3].name == String(u8"Bus 1"));
    CHECK(asset.custom[3].parent == String(u8"Master"));

    CHECK(editor::RenameBus(asset, 0, u8"Feet"));
    CHECK(asset.custom[2].parent == String(u8"Feet")); // its child followed
    CHECK_FALSE(editor::RenameBus(asset, 0, u8"Music"));    // a fixed bus's
    CHECK_FALSE(editor::RenameBus(asset, 0, u8"Ambience")); // another's
    CHECK_FALSE(editor::RenameBus(asset, 0, u8""));
    CHECK(asset.custom[0].name == String(u8"Feet"));

    CHECK_FALSE(editor::ReparentBus(asset, 0, u8"Steps")); // its own child: a loop
    CHECK(editor::ReparentBus(asset, 2, u8"Ambience"));
    CHECK(asset.custom[2].parent == String(u8"Ambience"));
    // Ambience may move under the four fixed buses, Feet and Bus 1: not itself, not Steps under it.
    CHECK(editor::BusParentChoices(asset, 1).Size() == 6u);

    editor::RemoveBus(asset, 1); // Ambience: Steps under it moves to Master
    CHECK(asset.custom[1].name.IsEmpty());
    CHECK(asset.custom[2].parent == String(u8"Master"));
}

namespace
{
    namespace gui = foundation::gui;

    RefPtr<gui::Node> LoadBusMarkup(StringView relative)
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

    // A project with the layout above ("Mixer").
    struct BusBed
    {
        String dir = String(u8"scratch_bus_layout_page_project");
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        foundation::content::Instance* mixer = nullptr;

        BusBed()
        {
            pipeline::RegisterAudioAssets();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            mixer = project->SourceDb().RootGroup()->CreateInstance(u8"Mixer", pipeline::AudioBusLayoutAsset::StaticType());
            REQUIRE(mixer != nullptr);
            pipeline::AudioBusLayoutAsset asset;
    FillLayout(asset);
            REQUIRE(mixer->WriteObject(asset).IsOk());
        }
        ~BusBed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<editor::AudioBusLayoutEditorPage> Open()
        {
            editor::app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadBusMarkup(path); };
            return MakeUnique<editor::AudioBusLayoutEditorPage>(DefaultAllocator(), context, Move(services), *mixer);
        }
    };
}

TEST_CASE("bus layout page: its tree, the chosen bus's rows: a fixed one's settings, a custom one's name and parent too")
{
    BusBed bed;
    UniquePtr<editor::AudioBusLayoutEditorPage> page = bed.Open();
    REQUIRE_FALSE(page->LoadFailed());
    CHECK(page->Tree() == String(u8"Master\n  Effects\n    Footsteps\n      Steps\n  Music\n    Ambience\n  UI"));
    CHECK(page->Selected().fixed == 0); // Master
    CHECK_FALSE(page->BusRows().FindInfo(u8"name")->Visible);
    CHECK(page->EffectRows(u8"Mix").GetValue(u8"volume", 0).AsFloat() == doctest::Approx(1.0));
    CHECK(page->EffectRows(u8"Reverb").FindInfo(u8"reverbWet") != nullptr); // a section per group

    page->Select(editor::BusRef{-1, 0}); // Footsteps
    CHECK(page->BusRows().FindInfo(u8"name")->Visible);
    CHECK(page->BusRows().GetValue(u8"name", 0).AsString() == StringView(u8"Footsteps"));
    const gui::PropertyInfo* parent = page->BusRows().FindInfo(u8"parent");
    REQUIRE(parent != nullptr);
    CHECK(parent->Choices.Size() == 5u); // the four fixed and Ambience: not itself, not Steps under it
    CHECK(parent->Choices[static_cast<usize>(page->BusRows().GetValue(u8"parent", 0).AsInt())].Name == StringView(u8"Effects"));
}

TEST_CASE("bus layout page: a setting, a rename and a move each one undo step; a bus added and removed")
{
    BusBed bed;
    UniquePtr<editor::AudioBusLayoutEditorPage> page = bed.Open();
    page->Select(editor::BusRef{-1, 0}); // Footsteps
    page->EditRow(page->EffectRows(u8"Reverb"), u8"reverbWet", gui::Variant(0.5));
    auto* layout = Cast<pipeline::AudioBusLayoutAsset>(page->Object());
    REQUIRE(layout != nullptr);
    CHECK(layout->custom[0].bus.reverbWet == doctest::Approx(0.5f));
    CHECK(page->IsDirty());

    page->EditRow(page->BusRows(), u8"name", gui::Variant(StringView(u8"Feet")));
    CHECK(layout->custom[2].parent == String(u8"Feet")); // Steps followed
    CHECK(page->Tree() == String(u8"Master\n  Effects\n    Feet\n      Steps\n  Music\n    Ambience\n  UI"));
    page->EditRow(page->BusRows(), u8"name", gui::Variant(StringView(u8"Music"))); // a fixed bus's: refused
    CHECK(layout->custom[0].name == String(u8"Feet"));

    // Moved under Music (its index among the choices offered).
    const gui::PropertyInfo* parent = page->BusRows().FindInfo(u8"parent");
    i64 music = -1;
    for (usize i = 0; i < parent->Choices.Size(); ++i)
        if (parent->Choices[i].Name == StringView(u8"Music"))
            music = static_cast<i64>(i);
    REQUIRE(music >= 0);
    page->EditRow(page->BusRows(), u8"parent", gui::Variant(music));
    CHECK(page->Tree() == String(u8"Master\n  Effects\n  Music\n    Feet\n      Steps\n    Ambience\n  UI"));

    page->Commands().Undo(); // the move
    page->Commands().Undo(); // the rename
    page->Commands().Undo(); // the setting
    CHECK(layout->custom[0].name == String(u8"Footsteps"));
    CHECK(layout->custom[0].bus.reverbWet == doctest::Approx(0.0f));

    REQUIRE(page->AddBus());
    CHECK(page->Selected().slot == 3);
    CHECK(layout->custom[3].name == String(u8"Bus 1"));
    REQUIRE(page->RemoveSelected());
    CHECK(layout->custom[3].name.IsEmpty());
    CHECK(page->Selected().fixed == 0); // Master chosen again
    page->Commands().Undo();
    CHECK(layout->custom[3].name == String(u8"Bus 1"));
}
