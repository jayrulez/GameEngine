// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :spacer partition
//
// Empty view for explicit spacing. Ported from Sedulous.UI/src/Controls/Spacer.bf.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
export module foundation.ui:spacer;
import foundation.core;
import :view;
import :property;
import :box_constraints;
using namespace foundation::core;
export namespace foundation::ui
{
    class Spacer : public View
    {
        RTTI_OBJECT(Spacer, View)
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
    RTTI_DEFINE_OBJECT(Spacer, "rtti::ui")
}
