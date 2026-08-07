// Single definition point for the shared test doubles (TestView/TestGroup).
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::foundation;

namespace draconic::ui::tests
{
    void TestView::OnMeasure(BoxConstraints constraints)
    {
        MeasuredSize = draconic::foundation::Float2{constraints.ConstrainWidth(DesiredWidth),
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

    DRACONIC_DEFINE_OBJECT(TestView, "draconic::ui::tests")
    DRACONIC_DEFINE_OBJECT(TestGroup, "draconic::ui::tests")
}
