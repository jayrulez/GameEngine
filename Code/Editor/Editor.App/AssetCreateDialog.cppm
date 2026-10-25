// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_create_dialog partition
//
// Create Asset (Documentation/Specs/editor-gui.md, 5.2, layer 5): a window over its markup
// (Data/Editor/AssetCreate.gml) that names a new asset and the group it goes in, for any type:
// it creates nothing itself, its owner does with what Create answers. The group is a field that
// opens the group picker below it; the name is checked as it is typed (AssetNames, the rule Save
// As keeps too), Create waiting while it is refused and the line under the name saying why, else
// what it will make. Enter in the name creates, Escape or Cancel closes.
module;
#include "Core/Prelude.h"

export module editor.app:asset_create_dialog;

import foundation.core;
import foundation.content;
import foundation.gui;
import :picker;
import :group_picker_dialog;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct AssetNames
    {
        /// Why `name` cannot name a new asset in `group` (empty, a character a file cannot hold,
        /// space at an end, taken there); empty when it can.
        [[nodiscard]] static String Refusal(const foundation::content::Group& group, StringView name)
        {
            if (name.IsEmpty())
                return String(u8"Enter a name.");
            for (const utf8char c : name)
                for (const utf8char refused : StringView(u8"/\\:*?\"<>|"))
                    if (c == refused)
                        return Format(u8"A name cannot hold {}.", StringView(&c, 1));
            if (name[0] == u8' ' || name[name.Size() - 1] == u8' ' || name[name.Size() - 1] == u8'.')
                return String(u8"A name cannot start or end with a space, or end with a dot.");
            if (group.GetInstance(name) != nullptr)
                return Format(u8"{} is already in {}.", name, GroupPicker::ShownName(group));
            return String{};
        }
    };

    class AssetCreate
    {
    public:
        static constexpr const char8_t* kPath = u8"Editor/AssetCreate.gml";
        static constexpr f32 kWidth = 440.0f;
        static constexpr f32 kHeight = 220.0f;

        /// Opens it over `root` with `body` (the markup's), titled `title` ("New Material"), the
        /// group `group` of `sources` to start, the name `suggested`; `picker` picks another group.
        /// `create` is told the group and the name Create was pressed with (a name it takes), then
        /// it closes. Null when `body` lacks its parts.
        static RefPtr<gui::Window> Open(IAllocator& allocator, gui::SceneRoot& root, RefPtr<gui::Node> body, foundation::content::ContentDatabase& sources,
                                        foundation::content::Group& group, StringView title, StringView suggested, Picker& picker,
                                        Function<void(foundation::content::Group&, StringView)> create)
        {
            if (!body)
                return {};
            auto* groupField = Cast<gui::Button>(FindIn(*body, u8"asset-create-group"));
            auto* name = Cast<gui::TextField>(FindIn(*body, u8"asset-create-name"));
            auto* why = Cast<gui::Label>(FindIn(*body, u8"asset-create-why"));
            auto* createButton = Cast<gui::Button>(FindIn(*body, u8"asset-create-create"));
            auto* cancel = Cast<gui::Button>(FindIn(*body, u8"asset-create-cancel"));
            if (groupField == nullptr || name == nullptr || why == nullptr || createButton == nullptr)
                return {};
            RefPtr<gui::Window> window = MakeRef<gui::Window>(allocator);
            window->SetTitle(title);
            window->AddClass(u8"asset-create-window");
            window->SetSize(Float2{kWidth, kHeight});
            window->GetContent()->AddChild(body.Get());

            // What the window holds while it is open: the group, and the owner's create.
            struct State : public RefCounted
            {
                foundation::content::Group* group = nullptr;
                Function<void(foundation::content::Group&, StringView)> create;
            };
            RefPtr<State> state = MakeRef<State>(allocator);
            state->group = &group;
            state->create = Move(create);
            gui::Window* raw = window.Get();
            const auto check = [state, groupField, name, why, createButton]()
            {
                groupField->SetText(GroupPicker::ShownName(*state->group).AsView());
                const String refused = AssetNames::Refusal(*state->group, name->GetText());
                createButton->SetEnabled(refused.IsEmpty());
                if (refused.IsEmpty())
                {
                    why->RemoveClass(u8"refused");
                    why->SetText(Format(u8"Makes {} in {}.", name->GetText(), GroupPicker::ShownName(*state->group)).AsView());
                }
                else
                {
                    why->AddClass(u8"refused");
                    why->SetText(refused.AsView());
                }
            };
            const auto commit = [state, raw, name]()
            {
                const String chosen(name->GetText());
                if (!AssetNames::Refusal(*state->group, chosen.AsView()).IsEmpty())
                    return; // Create waits; the line says why
                Function<void(foundation::content::Group&, StringView)> made = Move(state->create);
                foundation::content::Group* in = state->group;
                raw->Close();
                if (made)
                    made(*in, chosen.AsView());
            };
            foundation::content::ContentDatabase* db = &sources;
            Picker* groups = &picker;
            gui::SceneRoot* over = &root;
            groupField->SetOnClick([state, db, groups, over, check]()
                                   {
                                       PickerRequest request = GroupPicker::Request(*db, state->group);
                                       request.OnPicked = [state, db, check](StringView key)
                                       {
                                           if (foundation::content::Group* picked = GroupPicker::Find(*db, key))
                                           {
                                               state->group = picked;
                                               check();
                                           }
                                       };
                                       (void)groups->Open(*over, Move(request));
                                   });
            name->SetOnTextChanging([check](StringView) { check(); });
            (void)name->AddEventListener(gui::EventType::Submit, [commit](const gui::Event&) { commit(); });
            createButton->SetOnClick(commit);
            if (cancel != nullptr)
                cancel->SetOnClick([raw]() { raw->Close(); });
            name->SetText(suggested);
            check();
            window->OpenModal(root);
            name->RequestFocus();
            name->SelectAll();
            return window;
        }

    private:
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }
    };
}
