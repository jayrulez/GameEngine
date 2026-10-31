// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - the views' reflection and their registration as elements. Markup
// places the views, and the application sets their focus and axis in code.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.profiler.gui;

import foundation.core;
import foundation.gui;

namespace core = foundation::core;

namespace foundation::profiler::gui
{
    RTTI_DEFINE_OBJECT(ProfilerWidget, "rtti::profiler::gui")
    RTTI_DEFINE_OBJECT(FrameAxisView, "rtti::profiler::gui")
    REFLECT_MEMBERS(FrameStrip, "rtti::profiler::gui")
    {
        builder.Creator();
    }
    REFLECT_MEMBERS(CounterPlot, "rtti::profiler::gui")
    {
        builder.Creator();
    }
    REFLECT_MEMBERS(ProfileTimeline, "rtti::profiler::gui")
    {
        builder.Creator();
    }
    REFLECT_MEMBERS(ScopeTable, "rtti::profiler::gui")
    {
        builder.Creator();
    }
    REFLECT_MEMBERS(ScopeDetail, "rtti::profiler::gui")
    {
        builder.Creator();
    }
    REFLECT_MEMBERS(ProfilerOverlay, "rtti::profiler::gui")
    {
        builder.Creator();
    }

    void RegisterProfilerElements()
    {
        static const bool once = []()
        {
            const core::TypeInfo* const kElements[] = {
                &FrameStrip::StaticType(),
                &CounterPlot::StaticType(),
                &ProfileTimeline::StaticType(),
                &ScopeTable::StaticType(),
                &ScopeDetail::StaticType(),
            };
            for (const core::TypeInfo* type : kElements)
                (void)foundation::gui::ElementTypes::Register(*type);
            return true;
        }();
        (void)once;
    }
}
