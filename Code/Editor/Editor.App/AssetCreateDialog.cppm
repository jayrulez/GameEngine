// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_create_dialog partition
//
// Create Asset (Documentation/Specs/editor-gui.md, 5.2, layer 5): a window over its markup
// (Data/Editor/AssetCreate.xml) that names a new asset and the group it goes in, for any type:
// it creates nothing itself, its owner does with what Create answers. The groups are a tree in it,
// every one open, the group it starts in chosen and a click choosing another (the old dialog's);
// the name is checked as it is typed (AssetNames, the rule Save
// As keeps too), Create waiting while it is refused and the line under the name saying why, else
// what it will make. Enter in the name creates, Escape or Cancel closes.
module;
#include "Core/Prelude.h"

export module editor.app:asset_create_dialog;

import foundation.core;
import foundation.content;
import foundation.gui;
import :group_picker_dialog; // GroupPicker: a group's name and key

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

    /// AssetCreate::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kAssetCreatePath = u8"Editor/AssetCreate.xml";

    class AssetCreate
    {
    public:
        static constexpr const char8_t* kPath = kAssetCreatePath;
        static constexpr f32 kWidth = 440.0f;
        static constexpr f32 kHeight = 420.0f;

        /// Opens it over `root` with `body` (the markup's), titled `title` ("New Material"), the
        /// group `group` of `sources` chosen in its tree to start, the name `suggested`. `create`
        /// is told the group and the name Create was pressed with (a name it takes), then it
        /// closes. Null when `body` lacks its parts.
        static RefPtr<gui::Window> Open(IAllocator& allocator, gui::SceneRoot& root, RefPtr<gui::Node> body, foundation::content::ContentDatabase& sources,
                                        foundation::content::Group& group, StringView title, StringView suggested,
                                        Function<void(foundation::content::Group&, StringView)> create)
        {
            if (!body)
                return {};
            auto* tree = Cast<gui::TreeView>(FindIn(*body, u8"asset-create-groups"));
            auto* name = Cast<gui::TextField>(FindIn(*body, u8"asset-create-name"));
            auto* why = Cast<gui::Label>(FindIn(*body, u8"asset-create-why"));
            auto* createButton = Cast<gui::Button>(FindIn(*body, u8"asset-create-create"));
            auto* cancel = Cast<gui::Button>(FindIn(*body, u8"asset-create-cancel"));
            if (tree == nullptr || name == nullptr || why == nullptr || createButton == nullptr || sources.RootGroup() == nullptr)
                return {};
            RefPtr<gui::Window> window = MakeRef<gui::Window>(allocator);
            window->SetTitle(title);
            window->AddClass(u8"asset-create-window");
            window->SetSize(Float2{kWidth, kHeight});
            window->GetContent()->AddChild(body.Get());

            // What the window holds while it is open: the group, the tree's model and its groups by
            // node, and the owner's create.
            struct State : public RefCounted
            {
                explicit State(IAllocator& allocator) : groups(allocator) {}
                foundation::content::Group* group = nullptr;
                gui::TreeModel model;
                Array<foundation::content::Group*> groups; // by the tree's node
                Function<void(foundation::content::Group&, StringView)> create;
            };
            RefPtr<State> state = MakeRef<State>(allocator, allocator);
            state->group = &group;
            state->create = Move(create);
            AddGroups(state->model, state->groups, *sources.RootGroup(), gui::TreeModel::kRoot);
            tree->SetModel(&state->model);
            // A tree has no size of its own: room for a few groups whatever the sheet says.
            tree->EditLayoutStyle([](gui::LayoutStyle& s) { s.MinHeight = gui::LayoutLength::Px(160.0f); });
            for (usize node = 0; node < state->groups.Size(); ++node)
                tree->SetIdExpanded(static_cast<gui::ItemId>(node), true);
            for (usize node = 0; node < state->groups.Size(); ++node)
                if (state->groups[node] == &group)
                {
                    const gui::ItemId id = static_cast<gui::ItemId>(node);
                    tree->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
                }
            gui::Window* raw = window.Get();
            const auto check = [state, name, why, createButton]()
            {
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
            tree->SetOnSelectionChanged([state, check](gui::ModelIndex index)
                                        {
                                            const usize node = index.IsValid() ? static_cast<usize>(index.InternalId) : state->groups.Size();
                                            if (node >= state->groups.Size())
                                                return;
                                            state->group = state->groups[node];
                                            check();
                                        });
            window->SetOnClose([tree]() { tree->SetModel(nullptr); }); // the model goes with the window
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
        // `group` and every group under it, by name ("Content" the top), in the database's order.
        static void AddGroups(gui::TreeModel& model, Array<foundation::content::Group*>& groups, foundation::content::Group& group, i32 parent)
        {
            const i32 node = model.AddNode(parent, group.Parent() == nullptr ? GroupPicker::kTop : group.Name());
            model.SetIcon(node, u8"Editor/Icons/folder.svg");
            if (static_cast<usize>(node) >= groups.Size())
                groups.Resize(static_cast<usize>(node) + 1);
            groups[static_cast<usize>(node)] = &group;
            for (foundation::content::Group* child : group.Groups())
                AddGroups(model, groups, *child, node);
        }
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
