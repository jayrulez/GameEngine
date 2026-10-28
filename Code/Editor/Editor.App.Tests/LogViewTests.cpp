// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Console over its markup (Data/Editor/Console.gml): lines with their category, the levels'
// chips (Debug holding trace, Error holding fatal) with their counts showing and hiding
// their lines, the search, the cap trimming the oldest, Clear, the empty line, a selection kept
// by entry through a trim and copied oldest first, and each line's time.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;
import foundation.gui;
import editor.app;

using namespace foundation::core;
using namespace editor::app;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadConsole()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(LogView::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
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

    // A clipboard the test reads.
    class Clipboard final : public gui::IClipboard
    {
    public:
        void SetText(StringView text) override { held = String(text); }
        [[nodiscard]] String GetText() const override { return held; }
        [[nodiscard]] bool HasText() const override { return !held.IsEmpty(); }
        String held;
    };

    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        Clipboard clipboard;
        UniquePtr<LogView> view;
        Bed()
        {
            root->SetSize(Float2{800.0f, 400.0f});
            root->GetEventDispatcher()->SetClipboard(&clipboard);
            view = MakeUnique<LogView>(DefaultAllocator(), DefaultAllocator(), LoadConsole());
            REQUIRE(view->Content().Get() != nullptr);
            view->Content()->EditLayoutStyle([](gui::LayoutStyle& s)
            {
                s.Width = gui::LayoutLength::Px(800.0f);
                s.Height = gui::LayoutLength::Px(400.0f);
            });
            root->AddChild(view->Content().Get());
        }
        [[nodiscard]] gui::Button& Chip(StringView id) { return *FindId<gui::Button>(*view->Content(), id); }
    };
}

TEST_CASE("console: lines with their category, the levels in four chips with their counts")
{
    Bed bed;
    CHECK(FindId<gui::Label>(*bed.view->Content(), u8"console-empty")->IsDisplayed()); // nothing yet
    bed.view->AddEntry(LogLevel::Info, u8"Editor", u8"opened project");
    bed.view->AddEntry(LogLevel::Trace, u8"RHI", u8"trace detail");
    bed.view->AddEntry(LogLevel::Fatal, u8"RHI", u8"device lost");
    bed.view->AddEntry(LogLevel::Warning, u8"", u8"no category");
    CHECK(bed.view->EntryCount() == 4u);
    CHECK(bed.view->ShownCount() == 4u);
    CHECK(bed.view->ShownText(0) == StringView(u8"[Editor] opened project"));
    CHECK(bed.view->ShownText(3) == StringView(u8"no category"));
    CHECK_FALSE(FindId<gui::Label>(*bed.view->Content(), u8"console-empty")->IsDisplayed());

    CHECK(LogView::BucketOf(LogLevel::Trace) == LogView::Bucket::Debug);
    CHECK(LogView::BucketOf(LogLevel::Debug) == LogView::Bucket::Debug);
    CHECK(LogView::BucketOf(LogLevel::Fatal) == LogView::Bucket::Error);
    CHECK(bed.view->CountOf(LogView::Bucket::Info) == 1u);
    CHECK(bed.Chip(u8"console-info").GetText() == StringView(u8"Info 1"));
    CHECK(bed.Chip(u8"console-debug").GetText() == StringView(u8"Debug 1")); // the trace line
    CHECK(bed.Chip(u8"console-errors").GetText() == StringView(u8"Errors 1"));
}

TEST_CASE("console: a chip hides and shows its lines, in their order; the search narrows them")
{
    Bed bed;
    bed.view->AddEntry(LogLevel::Info, u8"A", u8"info");
    bed.view->AddEntry(LogLevel::Warning, u8"A", u8"warn");
    bed.view->AddEntry(LogLevel::Error, u8"A", u8"error");

    CHECK(bed.Chip(u8"console-warnings").HandleActivate()); // off
    CHECK_FALSE(bed.view->IsBucketVisible(LogView::Bucket::Warning));
    CHECK_FALSE(bed.Chip(u8"console-warnings").HasClass(u8"checked"));
    CHECK(bed.view->ShownCount() == 2u);
    CHECK(bed.view->ShownText(1) == StringView(u8"[A] error"));
    bed.view->AddEntry(LogLevel::Warning, u8"A", u8"warn2"); // hidden while its chip is off
    CHECK(bed.view->ShownCount() == 2u);
    CHECK(bed.Chip(u8"console-warnings").HandleActivate()); // on again: in order
    CHECK(bed.view->ShownCount() == 4u);
    CHECK(bed.view->ShownText(1) == StringView(u8"[A] warn"));
    CHECK(bed.view->ShownText(3) == StringView(u8"[A] warn2"));

    bed.view->SetSearch(u8"WARN");
    CHECK(bed.view->ShownCount() == 2u);
    bed.view->SetSearch(u8"[a]"); // the category is part of a line
    CHECK(bed.view->ShownCount() == 4u);
    bed.view->SetSearch(u8"nowhere");
    CHECK(bed.view->ShownCount() == 0u);
    CHECK(FindId<gui::Label>(*bed.view->Content(), u8"console-empty")->GetText() == StringView(u8"No line matches."));
    CHECK(FindId<gui::Button>(*bed.view->Content(), u8"console-clear-search")->HandleActivate());
    CHECK(bed.view->ShownCount() == 4u);
}

TEST_CASE("console: the cap trims the oldest, a selection kept by entry; Clear empties it")
{
    Bed bed;
    for (usize i = 0; i < LogView::kMaxEntries; ++i)
        bed.view->AddEntry(LogLevel::Info, u8"N", Format(u8"line {}", i).AsView());
    gui::ListView& list = *bed.view->List();
    list.SetSelectedItem(10); // "line 10"
    bed.view->AddEntry(LogLevel::Info, u8"N", u8"one more");
    CHECK(bed.view->EntryCount() == LogView::kMaxEntries);
    CHECK(bed.view->ShownText(0) == StringView(u8"[N] line 1"));
    CHECK(list.IsItemSelected(9)); // the same line, one up
    CHECK(bed.view->SelectedText() == StringView(u8"[N] line 10"));

    CHECK(FindId<gui::Button>(*bed.view->Content(), u8"console-clear")->HandleActivate());
    CHECK(bed.view->EntryCount() == 0u);
    CHECK(bed.view->CountOf(LogView::Bucket::Info) == 0u);
    CHECK(bed.Chip(u8"console-info").GetText() == StringView(u8"Info"));
}

TEST_CASE("console: the selected lines copied oldest first, one per line; Ctrl+C on the list")
{
    Bed bed;
    bed.view->AddEntry(LogLevel::Info, u8"A", u8"first");
    bed.view->AddEntry(LogLevel::Warning, u8"B", u8"second");
    bed.view->AddEntry(LogLevel::Error, u8"C", u8"third");
    usize copied = 0;
    bed.view->SetOnCopied([&copied](usize lines) { copied = lines; });
    gui::ListView& list = *bed.view->List();
    list.SelectAll();
    bed.view->CopySelection();
    CHECK(bed.clipboard.held == String(u8"[A] first\n[B] second\n[C] third"));
    CHECK(copied == 3u);

    list.ClearSelection();
    list.SetSelectedItem(2);
    list.RequestFocus();
    bed.root->GetEventDispatcher()->InjectKeyDown(static_cast<u32>(gui::KeyCode::C), gui::KeyModCtrl);
    CHECK(bed.clipboard.held == String(u8"[C] third"));
}

TEST_CASE("console: under the editor's sheet every line is one fixed height, so scrolling moves the bar rather than resizing it")
{
    Bed bed;
    String sheet;
    for (const char8_t* file : {u8"Editor/editor.css", u8"Editor/density-default.css"})
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
        REQUIRE(bytes.HasValue());
        sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        sheet += u8"\n";
    }
    gui::StyleManager styles;
    styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
    for (usize i = 0; i < 200; ++i)
        bed.view->AddEntry(LogLevel::Info, u8"N", Format(u8"line {}", i).AsView());
    styles.ApplyTree(*bed.root);
    (void)bed.root->UpdateLayout();
    gui::ListView& list = *bed.view->List();
    CHECK(list.GetRowHeight() == doctest::Approx(22.0f));
    const f32 range = list.GetScrollRange().y;
    list.ScrollToItem(0);
    (void)bed.root->UpdateLayout();
    CHECK(list.GetScrollRange().y == doctest::Approx(range)); // the content's height holds at the top
}

TEST_CASE("console: under the editor's sheet it keeps its panel's height and the wheel scrolls it up notch by notch, lines arriving meanwhile")
{
    Bed bed;
    String sheet;
    for (const char8_t* file : {u8"Editor/editor.css", u8"Editor/density-default.css"})
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
        REQUIRE(bytes.HasValue());
        sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        sheet += u8"\n";
    }
    gui::StyleManager styles;
    styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
    styles.ApplyTree(*bed.root);
    for (usize i = 0; i < 100; ++i)
        bed.view->AddEntry(LogLevel::Info, u8"N", Format(u8"line {}", i).AsView());
    styles.ApplyTree(*bed.root);
    (void)bed.root->UpdateLayout();
    gui::ListView& list = *bed.view->List();
    const gui::Rect box = list.GetScreenBounds();
    const Float2 at{box.x + 100.0f, box.y + box.height * 0.5f};
    gui::EventDispatcher& d = *bed.root->GetEventDispatcher();
    d.InjectMouseMove(at);
    list.ScrollToItem(static_cast<i32>(bed.view->ShownCount()) - 1); // at the newest, following it
    (void)bed.root->UpdateLayout();
    CHECK(list.GetScrollOffset().y == doctest::Approx(list.GetScrollRange().y));
    f32 last = list.GetScrollOffset().y;
    for (int notch = 0; notch < 6; ++notch)
    {
        (void)d.InjectMouseWheel(at, Float2{0.0f, 1.0f}); // up
        bed.view->AddEntry(LogLevel::Info, u8"N", Format(u8"more {}", notch).AsView());
        styles.ApplyTree(*bed.root);
        (void)bed.root->UpdateLayout();
        bed.root->GetContext().Update(Duration::FromSeconds(0.1));
        (void)bed.root->UpdateLayout();
        CHECK(list.GetSize().y < 400.0f); // its panel's, not its rows'
        CHECK(list.GetScrollRange().y > 0.0f);
        CHECK(list.GetScrollOffset().y < last); // up a notch, not pulled back down by the new line
        last = list.GetScrollOffset().y;
    }
}
