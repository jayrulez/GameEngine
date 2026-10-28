// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Single definition point for the shared test doubles (TestView/TestGroup).
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::core;

namespace foundation::ui::tests
{
    void TestView::OnMeasure(BoxConstraints constraints)
    {
        MeasuredSize = foundation::core::Float2{constraints.ConstrainWidth(DesiredWidth),
                                              constraints.ConstrainHeight(DesiredHeight)};
    }

    void TestGroup::OnLayout(f32 left, f32 top, f32 width, f32 height)
    {
        (void)left;
        (void)top;
        for (usize i = 0; i < ChildCount(); ++i)
        {
            View* child = GetChildAt(i);
            if (child->Visibility != Visibility::Gone)
            {
                child->Layout(0, 0, width, height);
            }
        }
    }

    RTTI_DEFINE_OBJECT(TestView, "rtti::ui::tests")
    RTTI_DEFINE_OBJECT(TestGroup, "rtti::ui::tests")
}
