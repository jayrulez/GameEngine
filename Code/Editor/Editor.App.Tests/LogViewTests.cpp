// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// LogView tests (headless): entry accumulation with category-prefixed text, level-bucket
// filtering (fold Trace+Debug / Error+Fatal), entry cap trimming, Clear, and copying a
// multi-row selection.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import editor.app;

using namespace foundation::core;
using namespace editor::app;

TEST_CASE("editor-logview: entries accumulate with category-prefixed, level-bucketed rows")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->AddEntry(LogLevel::Info, u8"Editor", u8"opened project");
    view->AddEntry(LogLevel::Trace, u8"RHI", u8"trace detail");
    view->AddEntry(LogLevel::Fatal, u8"RHI", u8"device lost");

    CHECK(view->EntryCount() == 3);
    CHECK(view->VisibleEntryCount() == 3);
    CHECK(view->VisibleEntryText(0) == u8"[Editor] opened project");

    CHECK(LogView::BucketOf(LogLevel::Trace) == LogView::Bucket::Debug);
    CHECK(LogView::BucketOf(LogLevel::Debug) == LogView::Bucket::Debug);
    CHECK(LogView::BucketOf(LogLevel::Info) == LogView::Bucket::Info);
    CHECK(LogView::BucketOf(LogLevel::Warning) == LogView::Bucket::Warning);
    CHECK(LogView::BucketOf(LogLevel::Error) == LogView::Bucket::Error);
    CHECK(LogView::BucketOf(LogLevel::Fatal) == LogView::Bucket::Error);
}

TEST_CASE("editor-logview: bucket filters hide and reshow entries")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->AddEntry(LogLevel::Info, u8"A", u8"info");
    view->AddEntry(LogLevel::Warning, u8"A", u8"warn");
    view->AddEntry(LogLevel::Error, u8"A", u8"error");

    view->SetBucketVisible(LogView::Bucket::Warning, false);
    CHECK(view->EntryCount() == 3);
    CHECK(view->VisibleEntryCount() == 2);
    CHECK(view->VisibleEntryText(1) == u8"[A] error");

    // Entries added while filtered out stay hidden...
    view->AddEntry(LogLevel::Warning, u8"A", u8"warn2");
    CHECK(view->VisibleEntryCount() == 2);

    // ...and reappear (in order) when the bucket is reshown.
    view->SetBucketVisible(LogView::Bucket::Warning, true);
    CHECK(view->VisibleEntryCount() == 4);
    CHECK(view->VisibleEntryText(1) == u8"[A] warn");
    CHECK(view->VisibleEntryText(3) == u8"[A] warn2");
}

TEST_CASE("editor-logview: entry cap trims oldest; Clear empties")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->MaxEntries = 3;
    view->AddEntry(LogLevel::Info, u8"A", u8"one");
    view->AddEntry(LogLevel::Info, u8"A", u8"two");
    view->AddEntry(LogLevel::Info, u8"A", u8"three");
    view->AddEntry(LogLevel::Info, u8"A", u8"four");

    CHECK(view->EntryCount() == 3);
    CHECK(view->VisibleEntryText(0) == u8"[A] two");
    CHECK(view->VisibleEntryText(2) == u8"[A] four");

    view->Clear();
    CHECK(view->EntryCount() == 0);
    CHECK(view->VisibleEntryCount() == 0);
}

namespace
{
    class TestClipboard final : public foundation::ui::IClipboard
    {
    public:
        [[nodiscard]] Status GetText(String& outText) override
        {
            outText = stored;
            return Status{};
        }
        [[nodiscard]] Status SetText(StringView text) override
        {
            stored = String(text);
            return Status{};
        }
        [[nodiscard]] bool HasText() override { return !stored.IsEmpty(); }
        String stored;
    };

    void Press(foundation::ui::ListView& list, foundation::ui::KeyCode key,
               foundation::ui::KeyModifiers modifiers)
    {
        foundation::ui::KeyEventArgs e;
        e.Key = key;
        e.Modifiers = modifiers;
        list.OnKeyDown(e);
        CHECK(e.Handled);
    }
}

// Sedulous 20bf699e and 714bfd5f: the console's rows select many; Ctrl+C copies them oldest
// first, one per line, and says how many; Ctrl+A selects every row; the cap's trim keeps the
// selection on its rows; a filter change clears it.
TEST_CASE("editor-logview: selected rows copy one per line, in order")
{
    namespace ui = foundation::ui;
    ui::UIContext context{DefaultAllocator()};
    TestClipboard clipboard;
    context.SetClipboard(&clipboard);
    auto root = MakeRef<ui::RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    context.AddRootView(root.Get());
    auto view = MakeRef<LogView>(DefaultAllocator());
    root->AddView(view.Get());
    usize copied = 0;
    view->OnCopied = [&](usize lines) { copied = lines; };

    view->AddEntry(LogLevel::Info, u8"A", u8"one");
    view->AddEntry(LogLevel::Warning, u8"A", u8"two");
    view->AddEntry(LogLevel::Error, u8"A", u8"three");

    // Several rows, picked out of order, copy oldest first.
    CHECK(view->List().Selection.Mode == ui::SelectionMode::Multiple);
    view->List().Selection.Toggle(2);
    view->List().Selection.Toggle(0);
    Press(view->List(), ui::KeyCode::C, ui::KeyModifiers::Ctrl);
    CHECK(clipboard.stored == u8"[A] one\n[A] three");
    CHECK(copied == 2u);

    // Ctrl+A selects every row, and copies them all.
    Press(view->List(), ui::KeyCode::A, ui::KeyModifiers::Ctrl);
    CHECK(view->SelectedCount() == 3u);
    Press(view->List(), ui::KeyCode::C, ui::KeyModifiers::Ctrl);
    CHECK(clipboard.stored == u8"[A] one\n[A] two\n[A] three");
    CHECK(copied == 3u);

    // The cap trims the oldest: the selection follows its rows, a trimmed one is gone.
    view->MaxEntries = 3;
    view->List().Selection.ClearSelection();
    view->List().Selection.Toggle(2); // "three"
    view->AddEntry(LogLevel::Info, u8"A", u8"four");
    CHECK(view->SelectedCount() == 1u);
    CHECK(view->SelectedText() == u8"[A] three");

    // A filter change re-numbers the rows, so the selection goes; nothing copies nothing.
    view->SetBucketVisible(LogView::Bucket::Warning, false);
    CHECK(view->SelectedCount() == 0u);
    clipboard.stored = String(u8"kept");
    Press(view->List(), ui::KeyCode::C, ui::KeyModifiers::Ctrl);
    CHECK(clipboard.stored == u8"kept");
    root->RemoveView(view.Get());
}

// Sedulous aa579970: the search shows the lines containing it, ignoring case, together with the
// level filters, and applies to lines that arrive while it is set.
TEST_CASE("editor-logview: the search narrows to matching lines")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->AddEntry(LogLevel::Info, u8"Editor", u8"opened scene 'Main'");
    view->AddEntry(LogLevel::Warning, u8"Resource", u8"bind failed for 6c26");
    view->AddEntry(LogLevel::Error, u8"Editor", u8"Cook FAILED");

    view->SetSearch(u8"failed");
    CHECK(view->VisibleEntryCount() == 2u); // either case
    CHECK(view->VisibleEntryText(0) == u8"[Resource] bind failed for 6c26");

    // Together with the level filters.
    view->SetBucketVisible(LogView::Bucket::Warning, false);
    CHECK(view->VisibleEntryCount() == 1u);
    CHECK(view->VisibleEntryText(0) == u8"[Editor] Cook FAILED");
    view->SetBucketVisible(LogView::Bucket::Warning, true);

    // A new line shows only when it matches; the category is part of the line.
    view->AddEntry(LogLevel::Info, u8"Editor", u8"saved");
    CHECK(view->VisibleEntryCount() == 2u);
    view->AddEntry(LogLevel::Info, u8"Editor", u8"retry failed");
    CHECK(view->VisibleEntryCount() == 3u);
    view->SetSearch(u8"resource]");
    CHECK(view->VisibleEntryCount() == 1u);

    // Empty shows everything again.
    view->SetSearch(u8"");
    CHECK(view->VisibleEntryCount() == 5u);
}

TEST_CASE("editor-logview: each level's chip counts its lines and shows or hides them")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->MaxEntries = 4;
    CHECK(view->LevelCountText(LogView::Bucket::Error) == StringView(u8"0"));
    view->AddEntry(LogLevel::Error, u8"A", u8"e1");
    view->AddEntry(LogLevel::Fatal, u8"A", u8"e2"); // Fatal counts as an error
    view->AddEntry(LogLevel::Warning, u8"A", u8"w1");
    view->AddEntry(LogLevel::Trace, u8"A", u8"t1"); // Trace counts as debug
    CHECK(view->LevelCount(LogView::Bucket::Error) == 2u);
    CHECK(view->LevelCount(LogView::Bucket::Warning) == 1u);
    CHECK(view->LevelCount(LogView::Bucket::Debug) == 1u);
    CHECK(view->LevelCountText(LogView::Bucket::Error) == StringView(u8"2"));

    // A line rolling off the cap leaves its level's count.
    view->AddEntry(LogLevel::Info, u8"A", u8"i1");
    CHECK(view->LevelCount(LogView::Bucket::Error) == 1u);
    CHECK(view->LevelCountText(LogView::Bucket::Error) == StringView(u8"1"));
    CHECK(view->LevelCount(LogView::Bucket::Info) == 1u);

    // A chip off hides its level's lines; on again shows them. The chip follows a call too.
    ui::ToggleButton* warnings = view->LevelChip(LogView::Bucket::Warning);
    REQUIRE(warnings != nullptr);
    CHECK(warnings->IsChecked.Value());
    warnings->IsChecked.SetValue(false);
    CHECK_FALSE(view->IsBucketVisible(LogView::Bucket::Warning));
    CHECK(view->VisibleEntryCount() == 3u);
    view->SetBucketVisible(LogView::Bucket::Warning, true);
    CHECK(warnings->IsChecked.Value());
    CHECK(view->VisibleEntryCount() == 4u);

    // Clear empties every count.
    view->Clear();
    for (usize i = 0; i < LogView::kBucketCount; ++i)
    {
        CHECK(view->LevelCount(static_cast<LogView::Bucket>(i)) == 0u);
    }
    CHECK(view->LevelCountText(LogView::Bucket::Info) == StringView(u8"0"));
}

TEST_CASE("editor-logview: the search field's x shows with text and clears it")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->AddEntry(LogLevel::Info, u8"A", u8"loaded");
    view->AddEntry(LogLevel::Info, u8"A", u8"failed");
    ui::IconButton* clear = view->ClearSearchButton();
    REQUIRE(clear != nullptr);
    CHECK(clear->Visibility == ui::VisibilityValue::Hidden);
    view->SetSearch(u8"fail");
    CHECK(view->VisibleEntryCount() == 1u);
    CHECK(clear->Visibility == ui::VisibilityValue::Visible);
    clear->FireClick();
    CHECK(view->Search().IsEmpty());
    CHECK(view->VisibleEntryCount() == 2u);
    CHECK(clear->Visibility == ui::VisibilityValue::Hidden);
}
