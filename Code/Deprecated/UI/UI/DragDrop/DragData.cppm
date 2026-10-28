// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :drag_data partition
//
// Base class for drag-and-drop payload data. Subclass to carry typed data; the Format string enables
// type matching between drag sources and drop targets. Ported from Sedulous.UI/src/DragDrop/DragData.bf.
// Per the locked decision, DragData derives Object + RTTI_OBJECT so subtype recovery uses our RTTI
// (Cast<T>) and the manager can own it via RefPtr (Beef `~delete _` -> RAII).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:drag_data;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui
{
    /// Base class for drag-and-drop payload data.
    class DragData : public Object
    {
        RTTI_OBJECT(DragData, Object)
    public:
        explicit DragData(StringView format) : m_format(format) {}

        /// Format string for type identification (e.g. "view/reorder", "text/plain").
        [[nodiscard]] StringView Format() const { return m_format.AsView(); }

    private:
        String m_format;
    };

    RTTI_DEFINE_OBJECT(DragData, "rtti::ui")
}
