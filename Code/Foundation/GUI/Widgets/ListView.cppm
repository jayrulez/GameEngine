// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :list_view partition
//
// ListView: a virtualised list over a model. Everything except the default row lives in
// ItemView. The default row is one label (`listcell`) showing the item's display text; a row
// factory (native code or a markup template) replaces it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:list_view;

import foundation.core; // RefPtr, MakeRef
import :node;
import :label;
import :text; // TextHAlign
import :model_index;
import :model;
import :item_view;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class ListView : public ItemView
    {
        RTTI_OBJECT(ListView, ItemView)
    public:
        ListView()
        {
            SetTag(core::StringView(u8"listview"));
            StyleWriteScope defaults(*this); // defaults: a style sheet overrides them and falls back to them
            SetTextColor(Color{0.88f, 0.90f, 0.94f, 1.0f});
        }

    protected:
        [[nodiscard]] core::StringView RowTag() const override { return core::StringView(u8"listviewrow"); }
        [[nodiscard]] core::RefPtr<Node> CreateContent(u32) override
        {
            auto label = core::MakeRef<Label>(MemoryAllocator());
            label->SetTag(core::StringView(u8"listcell")); // tagged so sheets style row labels apart from generic labels
            StyleWriteScope defaults(*label);
            label->SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
            label->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
            label->SetHitTestVisible(false); // presses go through to the row
            return label;
        }
        void BindContent(Node& content, i32, const ModelIndex& index) override
        {
            static_cast<Label&>(content).SetText(GetModel()->Data(index).ToString().AsView());
        }
    };
}
