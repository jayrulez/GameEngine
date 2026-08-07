// Shared test doubles for the View cluster (faithful port of Sedulous.UI.Tests/src/TestHelpers.bf).
// Declared here, defined once in TestHelpers.cpp (DRACONIC_OBJECT type-info must be single-definition).
// The including TU must `import draconic.ui;` before including this header.
#pragma once
#include "Draconic.Foundation/Reflection/Reflect.h"

namespace draconic::ui::tests
{
    /// Minimal concrete View with a fixed desired size.
    class TestView : public draconic::ui::View
    {
        DRACONIC_OBJECT(TestView, draconic::ui::View)
    public:
        draconic::foundation::f32 DesiredWidth = 50.0f;
        draconic::foundation::f32 DesiredHeight = 30.0f;

        TestView() = default;
        TestView(draconic::foundation::f32 w, draconic::foundation::f32 h) : DesiredWidth(w), DesiredHeight(h)
        {
        }

    protected:
        void OnMeasure(draconic::ui::BoxConstraints constraints) override;
    };

    /// Minimal concrete ViewGroup that lays out each child to fill its bounds.
    class TestGroup : public draconic::ui::ViewGroup
    {
        DRACONIC_OBJECT(TestGroup, draconic::ui::ViewGroup)
    protected:
        void OnLayout(draconic::foundation::f32 left, draconic::foundation::f32 top, draconic::foundation::f32 width,
                      draconic::foundation::f32 height) override;
    };

    /// Simple IListAdapter test double (Sedulous.UI.Tests SimpleListAdapter): a mutable Count and
    /// 100x30 TestView items. Not an Object, so it's header-inline (no DRACONIC_OBJECT needed).
    class SimpleListAdapter : public draconic::ui::ListAdapterBase
    {
    public:
        draconic::foundation::i32 Count = 0;
        explicit SimpleListAdapter(draconic::foundation::i32 count) : Count(count) {}
        [[nodiscard]] draconic::foundation::i32 ItemCount() const override { return Count; }
        [[nodiscard]] draconic::foundation::RefPtr<draconic::ui::View>
        CreateView(draconic::foundation::i32) override
        {
            return draconic::foundation::MakeRef<TestView>(draconic::foundation::DefaultAllocator(), 100.0f,
                                                     30.0f);
        }
        void BindView(draconic::ui::View*, draconic::foundation::i32) override {}
    };

    /// Test tree adapter (Sedulous.UI.Tests SimpleTreeAdapter): 3 roots; root 0 has 2 children (10,11),
    /// root 1 has 1 child (20), root 2 has none. Depth 1 for ids >= 10, else 0.
    class SimpleTreeAdapter : public draconic::ui::ITreeAdapter
    {
    public:
        [[nodiscard]] draconic::foundation::i32 RootCount() const override { return 3; }
        [[nodiscard]] draconic::foundation::i32 GetChildCount(draconic::foundation::i32 nodeId) const override
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
        [[nodiscard]] draconic::foundation::i32 GetChildId(draconic::foundation::i32 parentId,
                                                     draconic::foundation::i32 childIndex) const override
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
        [[nodiscard]] draconic::foundation::i32 GetDepth(draconic::foundation::i32 nodeId) const override
        {
            return nodeId >= 10 ? 1 : 0;
        }
        [[nodiscard]] bool HasChildren(draconic::foundation::i32 nodeId) const override
        {
            return nodeId == 0 || nodeId == 1;
        }
        [[nodiscard]] draconic::foundation::RefPtr<draconic::ui::View>
        CreateView(draconic::foundation::i32) override
        {
            return draconic::foundation::MakeRef<TestView>(draconic::foundation::DefaultAllocator(), 100.0f,
                                                     30.0f);
        }
        void BindView(draconic::ui::View*, draconic::foundation::i32, draconic::foundation::i32, bool) override
        {
        }
    };

    /// Sets up a UIContext + RootView (both owned by the caller).
    inline void Init(draconic::ui::UIContext& ctx, draconic::ui::RootView* root,
                     draconic::foundation::f32 width = 800.0f, draconic::foundation::f32 height = 600.0f)
    {
        root->ViewportSize = draconic::foundation::Float2{width, height};
        ctx.AddRootView(root);
    }

    inline void LayoutPass(draconic::ui::UIContext& ctx, draconic::ui::RootView* root)
    {
        ctx.BeginFrame(0.016f);
        ctx.UpdateRootView(root);
    }
}
