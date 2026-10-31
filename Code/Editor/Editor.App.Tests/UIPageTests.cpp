// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App: UIEditorPage. A page's content is a GUI node the shell docks, and the same node is
// returned each time it is asked for. Its frame hooks do nothing unless the page has work of its own.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace
{
    class NotePage final : public editor::app::UIEditorPage
    {
    public:
        explicit NotePage(IAllocator& allocator) : UIEditorPage(allocator), m_content(MakeRef<gui::Panel>(allocator)) {}
        [[nodiscard]] StringView Title() const override { return u8"Notes"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        [[nodiscard]] RefPtr<gui::Node> Content() override { return RefPtr<gui::Node>(m_content.Get()); }

    private:
        RefPtr<gui::Panel> m_content;
    };
}

TEST_CASE("ui page: its content is a GUI node, the same each time the dock asks")
{
    NotePage page(DefaultAllocator());
    const RefPtr<gui::Node> first = page.Content();
    REQUIRE(first);
    CHECK(page.Content().Get() == first.Get());
    page.OnClose(); // nothing of its own to release
}
