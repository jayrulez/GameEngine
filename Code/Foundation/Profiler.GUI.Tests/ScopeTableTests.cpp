// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Profiler.GUI - the scope table and the scope's detail (X7 of Specs/editor-gui.md): the call tree
// of the frames chosen (each thread a group, the GPU's passes one, a scope's runs in one place
// one row, self time its children's taken out); sorted by another heading a flat list of names;
// a filter showing the names holding its text and the rows they hang from; a span cutting the
// scopes to it; a click choosing a row's name, a click on an arrow folding it, on a heading
// sorting. The detail: the name's calls and times, its callers, its children by name with its own
// time last, its calls' spread over every frame held and its longest call in the frames chosen.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.profiler;
import foundation.profiler.gui;

#include "KnownSource.h"

using namespace foundation::core;
using namespace foundation::profiler;
using namespace foundation::profiler::gui;
using profiler_tests::KnownSource;
namespace fgui = foundation::gui;

namespace
{
    // A table 600 by 300 (headings 24 high, rows 22; the six number columns 72 wide from the end)
    // and a detail beside it, over frames 0 (20 ms) and 1 (10 ms), both chosen.
    struct Bed
    {
        KnownSource source;
        RefPtr<fgui::SceneRoot> root = MakeRef<fgui::SceneRoot>(DefaultAllocator(), MakeRef<fgui::GUIContext>(DefaultAllocator()));
        RefPtr<ProfileFocus> focus = MakeRef<ProfileFocus>(DefaultAllocator(), DefaultAllocator());
        RefPtr<ScopeTable> table = MakeRef<ScopeTable>(DefaultAllocator());
        RefPtr<ScopeDetail> detail = MakeRef<ScopeDetail>(DefaultAllocator());
        NameId frame = 0, update = 0, physics = 0, render = 0, job = 0, shadows = 0;

        Bed()
        {
            frame = source.Name(u8"Frame");
            update = source.Name(u8"Update");
            physics = source.Name(u8"Physics");
            render = source.Name(u8"Render");
            job = source.Name(u8"Job");
            shadows = source.Name(u8"Shadows");
            const u32 worker = source.Thread(u8"Worker");
            FrameRecord& f0 = source.AddFrame(20); // ticks 1000 to 1020
            KnownSource::AddScope(f0, physics, 2, 2, 10);
            KnownSource::AddScope(f0, physics, 2, 11, 14);
            KnownSource::AddScope(f0, update, 1, 1, 15);
            KnownSource::AddScope(f0, render, 1, 15, 19);
            KnownSource::AddScope(f0, frame, 0, 0, 20);
            KnownSource::AddScopeOn(f0, worker, job, 0, 2, 9);
            KnownSource::AddPass(f0, shadows, 3, 6);
            FrameRecord& f1 = source.AddFrame(10); // ticks 1020 to 1030
            KnownSource::AddScope(f1, physics, 2, 1, 5);
            KnownSource::AddScope(f1, update, 1, 0, 8);
            KnownSource::AddScope(f1, frame, 0, 0, 10);

            root->SetSize(Float2{1000.0f, 400.0f});
            root->EditLayoutStyle([](fgui::LayoutStyle& s)
            {
                s.Direction = fgui::FlexDirection::Row;
                s.AlignItems = fgui::Align::Start;
            });
            table->EditLayoutStyle([](fgui::LayoutStyle& s)
            {
                s.Width = fgui::LayoutLength::Px(600.0f);
                s.Height = fgui::LayoutLength::Px(300.0f);
            });
            detail->EditLayoutStyle([](fgui::LayoutStyle& s)
            {
                s.Width = fgui::LayoutLength::Px(380.0f);
                s.Height = fgui::LayoutLength::Px(300.0f);
            });
            root->AddChild(table.Get());
            root->AddChild(detail.Get());
            table->SetProfileFocus(focus);
            detail->SetProfileFocus(focus);
            focus->SetSource(&source);
            FrameRange both;
            both.first = 0;
            both.last = 1;
            both.empty = false;
            focus->SetFrames(both);
            (void)root->UpdateLayout();
        }
        fgui::EventDispatcher& D() { return *root->GetEventDispatcher(); }
        void Click(Float2 at)
        {
            D().InjectMouseMove(at);
            D().InjectMouseDown(at, fgui::MouseButton::Left);
            D().InjectMouseUp(at, fgui::MouseButton::Left);
        }
        void Pause() { Click(Float2{990.0f, 390.0f}); }
        static Float2 RowAt(usize row, f32 x = 100.0f) { return Float2{x, 24.0f + 22.0f * static_cast<f32>(row) + 11.0f}; }
        [[nodiscard]] String Labels() const
        {
            String out;
            for (const ScopeTable::Row& row : table->Rows())
            {
                if (!out.IsEmpty())
                    out += StringView(u8" ");
                out += row.label.AsView();
            }
            return out;
        }
        [[nodiscard]] const ScopeTable::Row* RowOf(StringView label) const
        {
            for (const ScopeTable::Row& row : table->Rows())
                if (row.label.AsView() == label)
                    return &row;
            return nullptr;
        }
    };
}

TEST_CASE("ScopeTable: the call tree of the frames chosen, a scope's runs in one place one row")
{
    Bed bed;
    CHECK(bed.Labels() == String(u8"Main Frame Update Physics Render Worker Job GPU Shadows"));
    const ScopeTable::Row* frame = bed.RowOf(u8"Frame");
    REQUIRE(frame != nullptr);
    CHECK(frame->calls == 2);
    CHECK(frame->depth == 1);
    CHECK(bed.table->CellText(*frame, ScopeColumn::Total) == String(u8"30.00"));
    CHECK(bed.table->CellText(*frame, ScopeColumn::Self) == String(u8"4.00")); // 30 less Update's 22 and Render's 4
    const ScopeTable::Row* physics = bed.RowOf(u8"Physics");
    REQUIRE(physics != nullptr);
    CHECK(physics->calls == 3);
    CHECK(bed.table->CellText(*physics, ScopeColumn::Total) == String(u8"15.00"));
    CHECK(bed.table->CellText(*physics, ScopeColumn::Mean) == String(u8"5.00"));
    CHECK(bed.table->CellText(*physics, ScopeColumn::Max) == String(u8"8.00"));
    CHECK(bed.table->CellText(*physics, ScopeColumn::Share) == String(u8"50.0%"));
    CHECK(bed.table->CellText(*bed.RowOf(u8"Main"), ScopeColumn::Total).IsEmpty()); // a group: no numbers
}

TEST_CASE("ScopeTable: another heading sorts a flat list of names, again the other way")
{
    Bed bed;
    bed.table->SortBy(ScopeColumn::Total);
    CHECK(bed.Labels() == String(u8"Frame Update Physics Job Render Shadows"));
    bed.table->SortBy(ScopeColumn::Total, false);
    CHECK(bed.Labels() == String(u8"Shadows Render Job Physics Update Frame"));
    bed.table->SortBy(ScopeColumn::Calls);
    CHECK(bed.table->Rows()[0].label == String(u8"Physics")); // three calls

    // By a click on the headings: Self (from the end, the fourth), then again the other way.
    bed.table->SortBy(ScopeColumn::Name);
    bed.Click(Float2{350.0f, 12.0f});
    CHECK(bed.table->SortColumn() == ScopeColumn::Self);
    CHECK(bed.table->SortsDescending());
    bed.Pause();
    bed.Click(Float2{350.0f, 12.0f});
    CHECK_FALSE(bed.table->SortsDescending());
    bed.Pause();
    bed.Click(Float2{60.0f, 12.0f});
    CHECK(bed.table->SortColumn() == ScopeColumn::Name);
}

TEST_CASE("ScopeTable: a filter shows the names holding its text, in the tree with the rows they hang from")
{
    Bed bed;
    bed.table->SetFilter(u8"PHYS");
    CHECK(bed.Labels() == String(u8"Main Frame Update Physics"));
    bed.table->SortBy(ScopeColumn::Total);
    CHECK(bed.Labels() == String(u8"Physics"));
}

TEST_CASE("ScopeTable: a span cuts the scopes to it; one that ran only outside it goes")
{
    Bed bed;
    bed.focus->SetSpan(1002, 1006);
    CHECK(bed.RowOf(u8"Render") == nullptr);
    const ScopeTable::Row* physics = bed.RowOf(u8"Physics");
    REQUIRE(physics != nullptr);
    CHECK(physics->calls == 1);
    CHECK(bed.table->CellText(*physics, ScopeColumn::Total) == String(u8"4.00"));
    CHECK(bed.table->CellText(*physics, ScopeColumn::Share) == String(u8"100.0%"));
    CHECK(bed.table->CellText(*bed.RowOf(u8"Shadows"), ScopeColumn::Total) == String(u8"3.00"));
    bed.focus->ClearSpan();
    CHECK(bed.RowOf(u8"Render") != nullptr);
}

TEST_CASE("ScopeTable: a click on a row chooses its name, on its arrow folds it")
{
    Bed bed;
    bed.Click(Bed::RowAt(3)); // Physics
    CHECK(bed.focus->Name().Is(bed.physics, false));
    bed.Pause();
    bed.Click(Bed::RowAt(8)); // Shadows, the GPU's
    CHECK(bed.focus->Name().Is(bed.shadows, true));
    bed.Pause();
    bed.Click(Bed::RowAt(1, 30.0f)); // Frame's arrow
    CHECK(bed.Labels() == String(u8"Main Frame Worker Job GPU Shadows"));
    CHECK(bed.focus->Name().Is(bed.shadows, true)); // folding chooses nothing
    bed.Pause();
    bed.Click(Bed::RowAt(1, 30.0f));
    CHECK(bed.table->Rows().Size() == 9);
}

TEST_CASE("ScopeDetail: the name chosen's calls, callers, children and spread; its longest call")
{
    Bed bed;
    CHECK_FALSE(bed.detail->Describe().any);
    bed.focus->ChooseName(bed.update, false);
    const ScopeDetail::Summary& update = bed.detail->Describe();
    REQUIRE(update.any);
    CHECK(update.calls == 2);
    CHECK(update.totalTicks == 22u);
    CHECK(update.maxTicks == 14u);
    CHECK(update.lanes == String(u8"Main"));
    REQUIRE(update.callers.Size() == 1);
    CHECK(update.callers[0] == String(u8"Frame (Main)"));
    REQUIRE(update.children.Size() == 2);
    CHECK(update.children[0].name == String(u8"Physics"));
    CHECK(update.children[0].ticks == 15u);
    CHECK(update.children[1].self);
    CHECK(update.children[1].ticks == 7u);
    CHECK(update.heldFrames == 2);
    CHECK(update.heldCalls == 2);
    u64 begin = 0;
    u64 end = 0;
    REQUIRE(bed.detail->LongestCall(begin, end));
    CHECK(begin == 1001u);
    CHECK(end == 1015u);

    bed.focus->ChooseName(bed.job, false);
    const ScopeDetail::Summary& job = bed.detail->Describe();
    REQUIRE(job.callers.Size() == 1);
    CHECK(job.callers[0] == String(u8"The top of Worker"));
    CHECK(job.lanes == String(u8"Worker"));

    // Its spread covers every frame held; only the frames chosen's calls are lit.
    bed.focus->ChooseName(bed.physics, false);
    bed.focus->SetFrame(1);
    const ScopeDetail::Summary& physics = bed.detail->Describe();
    CHECK(physics.calls == 1);
    CHECK(physics.heldCalls == 3);
    u32 lit = 0;
    u32 counted = 0;
    for (usize b = 0; b < physics.bins.Size(); ++b)
    {
        counted += physics.bins[b];
        lit += physics.lit[b];
    }
    CHECK(counted == 3);
    CHECK(lit == 1);
    REQUIRE(bed.detail->LongestCall(begin, end));
    CHECK(begin == 1021u);
}
