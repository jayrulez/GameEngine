// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The picker over its markup (Data/Editor/Picker.xml): rows under their sections, favourites first;
// the current value checked and lit, the foot saying Esc keeps it; Up and Down past the headings
// from the search, Enter picking and closing; a search narrowing (a name that starts with it
// first) or saying nothing matches, with a way back; nothing to pick saying what is missing and
// running the owner's next step; None picking nothing. The path picker: the files under a folder
// with the extensions asked, ignoring case, sorted, under their folders. A tree request: rows
// indented in its order, a search keeping the groups above each match.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import editor.app;

using namespace foundation::core;
using editor::app::PathPicker;
using editor::app::Picker;
using editor::app::PickerItem;
using editor::app::PickerRequest;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadPicker()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(Picker::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    PickerItem Item(const char8_t* key, const char8_t* name, const char8_t* section, bool favourite = false)
    {
        PickerItem item;
        item.Key = String(key);
        item.Name = String(name);
        item.Section = String(section);
        item.Detail = String(u8"MaterialAsset");
        item.Favourite = favourite;
        return item;
    }

    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        Picker picker{DefaultAllocator(), LoadPicker()};
        Array<String> picked;
        Bed() { root->SetSize(Float2{800.0f, 600.0f}); }

        PickerRequest Materials()
        {
            PickerRequest request;
            request.Placeholder = String(u8"Search materials");
            request.Items.PushBack(Item(u8"wood", u8"Wood", u8"Props"));
            request.Items.PushBack(Item(u8"wool", u8"Wool", u8"Props"));
            request.Items.PushBack(Item(u8"dark", u8"Wood_Dark", u8"Town", true));
            request.Items.PushBack(Item(u8"redwood", u8"Redwood", u8"Town"));
            request.Current = String(u8"wood");
            request.OnPicked = [this](StringView key) { picked.PushBack(String(key)); };
            return request;
        }
        void Open(PickerRequest request)
        {
            REQUIRE(picker.Open(*root, Move(request)));
            (void)root->UpdateLayout();
        }
        void Key(gui::KeyCode code) { root->GetEventDispatcher()->InjectKeyDown(static_cast<u32>(code)); }
        [[nodiscard]] String Rows() const
        {
            String out;
            for (usize i = 0; i < picker.RowCount(); ++i)
            {
                if (!out.IsEmpty())
                    out += u8" ";
                const bool header = picker.IsHeader(static_cast<i32>(i));
                if (header)
                    out += u8"#";
                out += picker.RowText(static_cast<i32>(i));
            }
            return out;
        }
    };
}

TEST_CASE("picker: rows under their sections, favourites first; the current lit; Up and Down past the headings; Enter picks and closes")
{
    Bed bed;
    bed.Open(bed.Materials());
    CHECK(bed.Rows() == String(u8"#Favourites Wood_Dark #Props Wood Wool #Town Wood_Dark Redwood"));
    CHECK(bed.picker.LitKey() == StringView(u8"wood"));
    CHECK(bed.picker.Hint() == StringView(u8"Enter picks, Esc keeps Wood"));
    REQUIRE(bed.picker.CloseButton() != nullptr);

    bed.Key(gui::KeyCode::Up); // over the Props heading to the favourite
    CHECK(bed.picker.LitKey() == StringView(u8"dark"));
    bed.Key(gui::KeyCode::Up); // nothing above: it stays
    CHECK(bed.picker.LitKey() == StringView(u8"dark"));
    bed.Key(gui::KeyCode::Down);
    bed.Key(gui::KeyCode::Down);
    bed.Key(gui::KeyCode::Down); // over the Town heading
    CHECK(bed.picker.LitKey() == StringView(u8"dark"));
    bed.Key(gui::KeyCode::Down);
    CHECK(bed.picker.LitKey() == StringView(u8"redwood"));
    bed.Key(gui::KeyCode::Return);
    REQUIRE(bed.picked.Size() == 1u);
    CHECK(bed.picked[0] == String(u8"redwood"));
    CHECK_FALSE(bed.picker.IsOpen());
}

TEST_CASE("picker: a search narrows, a name starting with it first; nothing matching says so and clears; None picks nothing")
{
    Bed bed;
    bed.Open(bed.Materials());
    bed.picker.SetQuery(u8"wood");
    CHECK(bed.Rows() == String(u8"#Favourites Wood_Dark #Props Wood #Town Wood_Dark Redwood")); // Redwood holds it: after
    bed.picker.SetQuery(u8"WOOL");
    CHECK(bed.Rows() == String(u8"#Props Wool")); // case folded; a section with nothing left goes
    bed.picker.SetQuery(u8"crate4");
    CHECK(bed.picker.RowCount() == 0u);
    CHECK(bed.picker.ShowsState());
    CHECK(bed.picker.StateTitle() == StringView(u8"Nothing matches crate4"));
    CHECK(bed.picker.StateVerbButton()->HandleActivate()); // Clear the search
    CHECK_FALSE(bed.picker.ShowsState());
    CHECK(bed.picker.RowCount() == 8u);

    // The close keeps the value: nothing picked, no None to slip onto.
    CHECK(bed.picker.CloseButton()->HandleActivate());
    CHECK(bed.picked.IsEmpty());
    CHECK_FALSE(bed.picker.IsOpen());
}

TEST_CASE("picker: a tree's rows indented in its order; a search keeps the groups above each match, dimmed, so two of one name read apart")
{
    Bed bed;
    PickerRequest request;
    const auto add = [&request](const char8_t* key, const char8_t* name, u32 depth)
    {
        PickerItem item;
        item.Key = String(key);
        item.Name = String(name);
        item.Depth = depth;
        request.Items.PushBack(Move(item));
    };
    add(u8"c", u8"Content", 0);
    add(u8"c/m", u8"Models", 1);
    add(u8"c/m/t", u8"Textures", 2);
    add(u8"c/u", u8"UI", 1);
    add(u8"c/u/t", u8"Textures", 2);
    add(u8"c/s", u8"Sounds", 1);
    request.Tree = true;
    request.Current = String(u8"c/u/t");
    bed.Open(Move(request));
    CHECK(bed.Rows() == String(u8"Content Models Textures UI Textures Sounds"));
    CHECK(bed.picker.RowDepth(2) == 2u);
    CHECK(bed.picker.LitKey() == StringView(u8"c/u/t"));
    CHECK_FALSE(bed.picker.IsContext(0));

    bed.picker.SetQuery(u8"tex");
    CHECK(bed.Rows() == String(u8"Content Models Textures UI Textures")); // tree order, Sounds gone
    CHECK(bed.picker.IsContext(0));
    CHECK(bed.picker.IsContext(1));
    CHECK_FALSE(bed.picker.IsContext(2));
    CHECK(bed.picker.LitKey() == StringView(u8"c/u/t")); // still the current

    bed.picker.SetQuery(u8"sound");
    CHECK(bed.Rows() == String(u8"Content Sounds"));
    CHECK(bed.picker.LitKey() == StringView(u8"c/s")); // the match lit, not what holds it
}

TEST_CASE("picker: nothing to pick says what is missing and runs the owner's next step after it closes")
{
    Bed bed;
    PickerRequest request;
    request.EmptyTitle = String(u8"No materials yet");
    request.EmptyText = String(u8"Import or create one.");
    request.EmptyVerb = String(u8"Create Material...");
    bool closedFirst = false;
    Picker* picker = &bed.picker;
    request.OnEmptyVerb = [&closedFirst, picker]() { closedFirst = !picker->IsOpen(); };
    bed.Open(Move(request));
    CHECK(bed.picker.ShowsState());
    CHECK(bed.picker.StateTitle() == StringView(u8"No materials yet"));
    CHECK(bed.picker.Hint() == StringView(u8"Enter picks, Esc closes"));
    CHECK(bed.picker.StateVerbButton()->GetText() == StringView(u8"Create Material..."));
    CHECK(bed.picker.StateVerbButton()->HandleActivate());
    CHECK(closedFirst);
}

TEST_CASE("path picker: the files under a folder with the extensions asked, ignoring case, sorted, under their folders")
{
    constexpr StringView kDir = u8"path-picker"; // under the harness's .test-scratch
    const auto write = [kDir](StringView relative)
    {
        const String full = PathJoin(kDir, relative);
        (void)CreateDirectories(PathParent(full.AsView()));
        REQUIRE(WriteFile(full.AsView(), Span<const byte>{}).IsOk());
    };
    write(u8"Fonts/Inter.TTF");
    write(u8"Fonts/Bold/Inter-Bold.ttf");
    write(u8"Arial.ttf");
    write(u8"readme.txt");
    const String ttf[] = {String(u8".ttf")};
    const PickerRequest request = PathPicker::Request(DefaultAllocator(), kDir, Span<const String>(ttf, 1u), u8"Fonts/Inter.TTF");
    REQUIRE(request.Items.Size() == 3u);
    CHECK(request.Items[0].Key == String(u8"Arial.ttf"));
    CHECK(request.Items[0].Section.IsEmpty());
    CHECK(request.Items[1].Key == String(u8"Fonts/Bold/Inter-Bold.ttf"));
    CHECK(request.Items[1].Name == String(u8"Inter-Bold.ttf"));
    CHECK(request.Items[1].Section == String(u8"Fonts/Bold"));
    CHECK(request.Items[2].Section == String(u8"Fonts"));
    CHECK(request.Current == String(u8"Fonts/Inter.TTF"));

    const String none[] = {String(u8".aeprofile")};
    const PickerRequest empty = PathPicker::Request(DefaultAllocator(), kDir, Span<const String>(none, 1u));
    CHECK(empty.Items.IsEmpty());
    CHECK(empty.EmptyTitle == String(u8"No .aeprofile files here"));
}

TEST_CASE("picker: under the editor's sheet its list fits its frame (a list wider than the dialog ran its rows past the edge)")
{
    Bed bed;
    String sheet;
    const char8_t* const files[] = {u8"Editor/editor.css", u8"Editor/density-default.css"};
    for (const char8_t* file : files)
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
        REQUIRE(bytes.HasValue());
        sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        sheet += u8"\n";
    }
    gui::StyleManager styles;
    styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
    bed.Open(bed.Materials());
    styles.ApplyTree(*bed.root);
    (void)bed.root->UpdateLayout();
    const gui::Rect frame = bed.picker.Content()->GetScreenBounds();
    const gui::Rect list = bed.picker.List()->GetScreenBounds();
    CHECK(list.x >= frame.x);
    CHECK(list.x + list.width <= frame.x + frame.width + 0.5f);
    bed.picker.Close();
}

TEST_CASE("picker: it opens in the window's middle; a lit item's description is the foot's, else what the keys do")
{
    Bed bed;
    PickerRequest request = bed.Materials();
    request.Items[1].Description = String(u8"Warm and soft."); // Wool's
    bed.Open(Move(request));
    const gui::Rect frame = bed.picker.Content()->GetScreenBounds();
    CHECK(frame.x + frame.width * 0.5f == doctest::Approx(400.0f).epsilon(0.01));
    CHECK(frame.y + frame.height * 0.5f == doctest::Approx(300.0f).epsilon(0.01));
    CHECK(bed.picker.Hint() == StringView(u8"Enter picks, Esc keeps Wood")); // Wood lit: it says nothing
    bed.Key(gui::KeyCode::Down);                                              // Wool
    CHECK(bed.picker.LitKey() == StringView(u8"wool"));
    CHECK(bed.picker.Hint() == StringView(u8"Warm and soft."));
    bed.Key(gui::KeyCode::Up);
    CHECK(bed.picker.Hint() == StringView(u8"Enter picks, Esc keeps Wood"));
    bed.picker.Close();
}
