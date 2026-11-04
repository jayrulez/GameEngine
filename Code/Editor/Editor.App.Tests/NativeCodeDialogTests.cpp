// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Add Native Code's dialog over its markup (Data/Editor/AddNativeCode.xml): a check box per part of
// the game's package. For a package not made yet Engine starts checked; a part the package has is
// checked and cannot be unchecked; Add is enabled while a part the package lacks is checked and
// hands those parts, in load order, to the request.

#include <doctest/doctest.h>
#include <initializer_list>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.runtime;
import editor.app;

using namespace foundation::core;
using editor::app::NativeCodeDialog;
using editor::app::NativeCodeRequest;
using foundation::runtime::PluginPartKind;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadDialog()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(),
                                     StringView(editor::app::kNativeCodeDialogPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    gui::Node* FindIn(gui::Node& node, StringView id)
    {
        if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return &node;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::Node> body = LoadDialog();
        RefPtr<gui::Window> window;
        Array<PluginPartKind> accepted;
        bool answered = false;

        explicit Bed(std::initializer_list<PluginPartKind> existing)
        {
            root->SetSize(Float2{900.0f, 700.0f});
            NativeCodeRequest request;
            request.path = String(u8"Plugins/MyGame");
            for (PluginPartKind kind : existing)
                request.existing.PushBack(kind);
            request.accepted = [this](Span<const PluginPartKind> parts)
            {
                answered = true;
                for (PluginPartKind kind : parts)
                    accepted.PushBack(kind);
            };
            window = NativeCodeDialog::Open(DefaultAllocator(), *root, body, Move(request));
            REQUIRE(window);
        }
        gui::CheckBox& Box(PluginPartKind kind) { return *Cast<gui::CheckBox>(FindIn(*body, NativeCodeDialog::BoxId(kind).AsView())); }
        gui::Button& Add() { return *Cast<gui::Button>(FindIn(*body, u8"native-code-add")); }
        StringView What() { return Cast<gui::Label>(FindIn(*body, u8"native-code-what"))->GetText(); }
    };
}

TEST_CASE("add native code: a new package starts with Engine checked, and Add makes the parts checked")
{
    Bed bed({});
    CHECK(bed.Box(PluginPartKind::Engine).IsChecked());
    CHECK_FALSE(bed.Box(PluginPartKind::Pipeline).IsChecked());
    CHECK(bed.Add().IsEnabled());
    CHECK(bed.What() == StringView(u8"Makes Plugins/MyGame with its Engine part, and enables it."));

    (void)bed.Box(PluginPartKind::Editor).HandleActivate(); // checked by the user
    CHECK(bed.What() == StringView(u8"Makes Plugins/MyGame with its Engine, Editor parts, and enables it."));
    (void)bed.Box(PluginPartKind::Engine).HandleActivate(); // and Engine unchecked
    (void)bed.Box(PluginPartKind::Engine).HandleActivate(); // and checked again
    (void)bed.Add().HandleActivate();
    CHECK(bed.answered);
    REQUIRE(bed.accepted.Size() == 2u);
    CHECK(bed.accepted[0] == PluginPartKind::Engine); // in load order, whatever order they were checked in
    CHECK(bed.accepted[1] == PluginPartKind::Editor);
    CHECK_FALSE(bed.window->IsOpen());
}

TEST_CASE("add native code: a package's parts show checked and fixed, and Add waits for a part it lacks")
{
    Bed bed({PluginPartKind::Engine});
    CHECK(bed.Box(PluginPartKind::Engine).IsChecked());
    CHECK_FALSE(bed.Box(PluginPartKind::Engine).IsEnabled());
    CHECK_FALSE(bed.Add().IsEnabled());
    CHECK(bed.What() == StringView(u8"Check a part the package does not have yet."));

    (void)bed.Box(PluginPartKind::Pipeline).HandleActivate();
    CHECK(bed.Add().IsEnabled());
    CHECK(bed.What() == StringView(u8"Adds the Pipeline part to Plugins/MyGame."));
    (void)bed.Add().HandleActivate();
    REQUIRE(bed.accepted.Size() == 1u); // Engine is there already
    CHECK(bed.accepted[0] == PluginPartKind::Pipeline);
}
