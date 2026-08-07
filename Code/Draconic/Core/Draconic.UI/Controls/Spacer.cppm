// Draconic UI - :spacer partition
//
// Empty view for explicit spacing. Ported from Sedulous.UI/src/Controls/Spacer.bf.
module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
export module draconic.ui:spacer;
import draconic.foundation;
import :view;
import :property;
import :box_constraints;
using namespace draconic::foundation;
export namespace draconic::ui
{
    class Spacer : public View
    {
        DRACONIC_OBJECT(Spacer, View)
    public:
        Property<f32> SpacerWidth{0.0f};
        Property<f32> SpacerHeight{0.0f};

        explicit Spacer(f32 width = 0.0f, f32 height = 0.0f)
        {
            SpacerWidth.SetOwner(this);
            SpacerHeight.SetOwner(this);
            SpacerWidth.SetSilent(width);
            SpacerHeight.SetSilent(height);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(SpacerWidth.Value()),
                                  constraints.ConstrainHeight(SpacerHeight.Value())};
        }
    };
    DRACONIC_DEFINE_OBJECT(Spacer, "draconic::ui")
}
