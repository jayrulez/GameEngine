// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :name_prompt partition
//
// The name prompt: a small window over its markup (Data/Editor/NamePrompt.xml) that asks for one
// name, for whatever is about to be made where the user already is (a new group in the Assets
// panel or the group picker, a new asset in the Assets panel). It makes nothing itself: its
// request says how a name is refused and what accepting makes, and gets the accepted name. The
// name is checked as it is typed; while it is refused the accept button does nothing and the line
// under the field says why, and otherwise that line says what will be made. Enter accepts; Escape
// or Cancel closes.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:name_prompt;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// What a name prompt asks for.
    struct NameRequest
    {
        String title;      ///< the window's title ("New Group in Models")
        String suggested;  ///< the name it starts with, selected so typing replaces it
        String accept;     ///< the accept button's text ("Create")
        /// Why a name is refused (empty when it is accepted).
        Function<String(StringView)> refusal;
        /// What accepting `name` makes ("Makes the group Props in Models."); unset says nothing.
        Function<String(StringView)> describe;
        /// Gets the accepted name, after the prompt has closed.
        Function<void(StringView)> accepted;
    };

    /// NamePrompt::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kNamePromptPath = u8"Editor/NamePrompt.xml";

    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct NamePrompt
    {
        static constexpr const char8_t* kPath = kNamePromptPath;
        static constexpr f32 kWidth = 400.0f;
        static constexpr f32 kHeight = 160.0f;

        /// Opens the prompt over `root` with `body` (loaded from the markup) for `request`; null
        /// when `body` lacks its parts.
        static RefPtr<gui::Window> Open(IAllocator& allocator, gui::SceneRoot& root, RefPtr<gui::Node> body, NameRequest request)
        {
            if (!body)
                return {};
            auto* name = Cast<gui::TextField>(FindIn(*body, u8"name-prompt-name"));
            auto* why = Cast<gui::Label>(FindIn(*body, u8"name-prompt-why"));
            auto* accept = Cast<gui::Button>(FindIn(*body, u8"name-prompt-accept"));
            auto* cancel = Cast<gui::Button>(FindIn(*body, u8"name-prompt-cancel"));
            if (name == nullptr || why == nullptr || accept == nullptr)
                return {};
            RefPtr<gui::Window> window = MakeRef<gui::Window>(allocator);
            window->SetTitle(request.title.AsView());
            window->AddClass(u8"name-prompt-window");
            window->SetSize(Float2{kWidth, kHeight});
            window->GetContent()->AddChild(body.Get());
            if (!request.accept.IsEmpty())
                accept->SetText(request.accept.AsView());

            struct State : public RefCounted
            {
                NameRequest request;
            };
            RefPtr<State> state = MakeRef<State>(allocator);
            const String suggested = request.suggested;
            state->request = Move(request);
            gui::Window* raw = window.Get();
            const auto refusalOf = [state](StringView text) { return state->request.refusal ? state->request.refusal(text) : String{}; };
            const auto check = [state, name, why, accept, refusalOf]()
            {
                const String refused = refusalOf(name->GetText());
                accept->SetEnabled(refused.IsEmpty());
                if (refused.IsEmpty())
                {
                    why->RemoveClass(u8"refused");
                    why->SetText(state->request.describe ? state->request.describe(name->GetText()).AsView() : StringView{});
                }
                else
                {
                    why->AddClass(u8"refused");
                    why->SetText(refused.AsView());
                }
            };
            const auto commit = [state, raw, name, refusalOf]()
            {
                const String chosen(name->GetText());
                if (!refusalOf(chosen.AsView()).IsEmpty())
                    return; // refused: the line under the field says why
                Function<void(StringView)> accepted = Move(state->request.accepted);
                raw->Close();
                if (accepted)
                    accepted(chosen.AsView());
            };
            name->SetOnTextChanging([check](StringView) { check(); });
            (void)name->AddEventListener(gui::EventType::Submit, [commit](const gui::Event&) { commit(); });
            accept->SetOnClick(commit);
            if (cancel != nullptr)
                cancel->SetOnClick([raw]() { raw->Close(); });
            name->SetText(suggested.AsView());
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
