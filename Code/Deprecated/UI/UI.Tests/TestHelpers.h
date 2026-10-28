// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Shared test doubles for the View cluster (faithful port of Sedulous.UI.Tests/src/TestHelpers.bf).
// Declared here, defined once in TestHelpers.cpp (RTTI_OBJECT type-info must be single-definition).
// The including TU must `import foundation.ui;` before including this header.
#pragma once
#include "Core/Reflection/Reflect.h"

namespace foundation::ui::tests
{
    /// Minimal concrete View with a fixed desired size.
    class TestView : public foundation::ui::View
    {
        RTTI_OBJECT(TestView, foundation::ui::View)
    public:
        foundation::core::f32 DesiredWidth = 50.0f;
        foundation::core::f32 DesiredHeight = 30.0f;

        TestView() = default;
        TestView(foundation::core::f32 w, foundation::core::f32 h) : DesiredWidth(w), DesiredHeight(h)
        {
        }

    protected:
        void OnMeasure(foundation::ui::BoxConstraints constraints) override;
    };

    /// Minimal concrete ViewGroup that lays out each child to fill its bounds.
    class TestGroup : public foundation::ui::ViewGroup
    {
        RTTI_OBJECT(TestGroup, foundation::ui::ViewGroup)
    protected:
        void OnLayout(foundation::core::f32 left, foundation::core::f32 top, foundation::core::f32 width,
                      foundation::core::f32 height) override;
    };

    /// Simple IListAdapter test double (Sedulous.UI.Tests SimpleListAdapter): a mutable Count and
    /// 100x30 TestView items. Not an Object, so it's header-inline (no RTTI_OBJECT needed).
    class SimpleListAdapter : public foundation::ui::ListAdapterBase
    {
    public:
        foundation::core::i32 Count = 0;
        explicit SimpleListAdapter(foundation::core::i32 count) : Count(count) {}
        [[nodiscard]] foundation::core::i32 ItemCount() const override { return Count; }
        [[nodiscard]] foundation::core::RefPtr<foundation::ui::View>
        CreateView(foundation::core::i32) override
        {
            return foundation::core::MakeRef<TestView>(foundation::core::DefaultAllocator(), 100.0f,
                                                     30.0f);
        }
        void BindView(foundation::ui::View*, foundation::core::i32) override {}
    };

    /// Test tree adapter (Sedulous.UI.Tests SimpleTreeAdapter): 3 roots; root 0 has 2 children (10,11),
    /// root 1 has 1 child (20), root 2 has none. Depth 1 for ids >= 10, else 0.
    class SimpleTreeAdapter : public foundation::ui::ITreeAdapter
    {
    public:
        [[nodiscard]] foundation::core::i32 RootCount() const override { return 3; }
        [[nodiscard]] foundation::core::i32 GetChildCount(foundation::core::i32 nodeId) const override
        {
            if (nodeId == -1)
            {
                return 3;
            }
            if (nodeId == 0)
            {
                return 2;
            }
            if (nodeId == 1)
            {
                return 1;
            }
            return 0;
        }
        [[nodiscard]] foundation::core::i32 GetChildId(foundation::core::i32 parentId,
                                                     foundation::core::i32 childIndex) const override
        {
            if (parentId == -1)
            {
                return childIndex;
            } // roots: 0, 1, 2
            if (parentId == 0)
            {
                return 10 + childIndex;
            } // 10, 11
            if (parentId == 1)
            {
                return 20 + childIndex;
            } // 20
            return -1;
        }
        [[nodiscard]] foundation::core::i32 GetDepth(foundation::core::i32 nodeId) const override
        {
            return nodeId >= 10 ? 1 : 0;
        }
        [[nodiscard]] bool HasChildren(foundation::core::i32 nodeId) const override
        {
            return nodeId == 0 || nodeId == 1;
        }
        [[nodiscard]] foundation::core::RefPtr<foundation::ui::View>
        CreateView(foundation::core::i32) override
        {
            return foundation::core::MakeRef<TestView>(foundation::core::DefaultAllocator(), 100.0f,
                                                     30.0f);
        }
        void BindView(foundation::ui::View*, foundation::core::i32, foundation::core::i32, bool) override
        {
        }
    };

    /// Sets up a UIContext + RootView (both owned by the caller).
    inline void Init(foundation::ui::UIContext& ctx, foundation::ui::RootView* root,
                     foundation::core::f32 width = 800.0f, foundation::core::f32 height = 600.0f)
    {
        root->ViewportSize = foundation::core::Float2{width, height};
        ctx.AddRootView(root);
    }

    inline void LayoutPass(foundation::ui::UIContext& ctx, foundation::ui::RootView* root)
    {
        ctx.BeginFrame(0.016f);
        ctx.UpdateRootView(root);
    }
}
