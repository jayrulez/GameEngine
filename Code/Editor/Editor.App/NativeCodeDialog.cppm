// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :native_code_dialog partition
//
// Add Native Code's dialog: a small window over its markup (Data/Editor/AddNativeCode.xml) that
// asks which parts of the game's plugin package to scaffold, a check box per part kind. A part the
// package already has shows checked and cannot be unchecked; Engine starts checked for a package
// not made yet. Add is enabled while a part the package lacks is checked, and the line under the
// boxes says what it makes. It makes nothing itself: Add hands the parts to make to the request.
// Escape or Cancel closes.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:native_code_dialog;

import foundation.core;
import foundation.gui;
import foundation.runtime; // PluginPartKind

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// What Add Native Code's dialog asks about.
    struct NativeCodeRequest
    {
        String path; ///< the package's path in the project ("Plugins/MyGame")
        Array<foundation::runtime::PluginPartKind> existing; ///< the parts the package has (none: not made yet)
        /// Gets the parts to make, in load order, after the dialog has closed.
        Function<void(Span<const foundation::runtime::PluginPartKind>)> accepted;
    };

    /// NativeCodeDialog::kPath, at namespace scope for another library to read (MSVC exports no
    /// class's static constexpr member from a shared library).
    inline constexpr const char8_t* kNativeCodeDialogPath = u8"Editor/AddNativeCode.xml";

    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct NativeCodeDialog
    {
        static constexpr const char8_t* kPath = kNativeCodeDialogPath;
        static constexpr f32 kWidth = 560.0f;
        static constexpr f32 kHeight = 260.0f;

        /// The check box of `kind`'s id in the markup.
        [[nodiscard]] static String BoxId(foundation::runtime::PluginPartKind kind)
        {
            String id(u8"native-code-");
            for (char8_t c : foundation::runtime::PluginPartKindName(kind))
                id.PushBack((c >= u8'A' && c <= u8'Z') ? static_cast<char8_t>(c - u8'A' + u8'a') : c);
            return id;
        }

        /// Opens the dialog over `root` with `body` (loaded from the markup) for `request`; null when
        /// `body` lacks its parts.
        static RefPtr<gui::Window> Open(IAllocator& allocator, gui::SceneRoot& root, RefPtr<gui::Node> body, NativeCodeRequest request)
        {
            if (!body)
                return {};
            auto* what = Cast<gui::Label>(FindIn(*body, u8"native-code-what"));
            auto* add = Cast<gui::Button>(FindIn(*body, u8"native-code-add"));
            auto* cancel = Cast<gui::Button>(FindIn(*body, u8"native-code-cancel"));
            gui::CheckBox* boxes[4] = {};
            for (usize i = 0; i < 4; ++i)
            {
                boxes[i] = Cast<gui::CheckBox>(FindIn(*body, BoxId(foundation::runtime::kPluginPartKinds[i]).AsView()));
                if (boxes[i] == nullptr)
                    return {};
            }
            if (what == nullptr || add == nullptr)
                return {};
            RefPtr<gui::Window> window = MakeRef<gui::Window>(allocator);
            window->SetTitle(request.existing.IsEmpty() ? u8"Add Native Code" : u8"Add Native Code Parts");
            window->AddClass(u8"add-native-code-window");
            window->SetSize(Float2{kWidth, kHeight});
            window->GetContent()->AddChild(body.Get());

            struct State : public RefCounted
            {
                NativeCodeRequest request;
                gui::CheckBox* boxes[4] = {};
            };
            RefPtr<State> state = MakeRef<State>(allocator);
            state->request = Move(request);
            const auto has = [state](foundation::runtime::PluginPartKind kind)
            {
                for (foundation::runtime::PluginPartKind existing : state->request.existing)
                    if (existing == kind)
                        return true;
                return false;
            };
            for (usize i = 0; i < 4; ++i)
            {
                state->boxes[i] = boxes[i];
                const foundation::runtime::PluginPartKind kind = foundation::runtime::kPluginPartKinds[i];
                const bool there = has(kind);
                boxes[i]->SetChecked(there || (state->request.existing.IsEmpty() && kind == foundation::runtime::PluginPartKind::Engine));
                boxes[i]->SetEnabled(!there);
            }
            // The parts checked that the package lacks, in load order.
            const auto chosen = [state, has]()
            {
                Array<foundation::runtime::PluginPartKind> parts;
                for (usize i = 0; i < 4; ++i)
                    if (state->boxes[i]->IsChecked() && !has(foundation::runtime::kPluginPartKinds[i]))
                        parts.PushBack(foundation::runtime::kPluginPartKinds[i]);
                return parts;
            };
            const auto check = [state, chosen, what, add]()
            {
                const Array<foundation::runtime::PluginPartKind> parts = chosen();
                add->SetEnabled(!parts.IsEmpty());
                if (parts.IsEmpty())
                {
                    what->SetText(u8"Check a part the package does not have yet.");
                    return;
                }
                String names;
                for (foundation::runtime::PluginPartKind kind : parts)
                {
                    names += names.IsEmpty() ? u8"" : u8", ";
                    names += foundation::runtime::PluginPartKindName(kind);
                }
                const StringView plural = parts.Size() == 1 ? StringView(u8"") : StringView(u8"s");
                what->SetText(state->request.existing.IsEmpty()
                                  ? Format(u8"Makes {} with its {} part{}, and enables it.", state->request.path, names, plural).AsView()
                                  : Format(u8"Adds the {} part{} to {}.", names, plural, state->request.path).AsView());
            };
            gui::Window* raw = window.Get();
            for (gui::CheckBox* box : boxes)
                box->SetOnToggled([check](bool) { check(); });
            add->SetOnClick(
                [state, raw, chosen]()
                {
                    const Array<foundation::runtime::PluginPartKind> parts = chosen();
                    if (parts.IsEmpty())
                        return;
                    Function<void(Span<const foundation::runtime::PluginPartKind>)> accepted = Move(state->request.accepted);
                    raw->Close();
                    if (accepted)
                        accepted(Span<const foundation::runtime::PluginPartKind>(parts.Data(), parts.Size()));
                });
            if (cancel != nullptr)
                cancel->SetOnClick([raw]() { raw->Close(); });
            check();
            window->OpenModal(root);
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
